/* Copyright (C) 2004 MySQL AB & MySQL Finland AB & TCX DataKonsult AB

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <my_time.h>
#include <m_string.h>
#include <m_ctype.h>

ulonglong log_10_int[20]=
{
  1, 10, 100, 1000, 10000UL, 100000UL, 1000000UL, 10000000UL,
  ULL(100000000), ULL(1000000000), ULL(10000000000), ULL(100000000000),
  ULL(1000000000000), ULL(10000000000000), ULL(100000000000000),
  ULL(1000000000000000), ULL(10000000000000000), ULL(100000000000000000),
  ULL(1000000000000000000), ULL(10000000000000000000)
};


/* Position for YYYY-DD-MM HH-MM-DD.FFFFFF AM in default format */

static uchar internal_format_positions[]=
{0, 1, 2, 3, 4, 5, 6, (uchar) 255};

static char time_separator=':';

/*
  Convert a timestamp string to a MYSQL_TIME value.

  SYNOPSIS
    str_to_datetime()
    str                 String to parse
    length              Length of string
    l_time              Date is stored here
    flags               Bitmap of following items
                        TIME_FUZZY_DATE    Set if we should allow partial dates
                        TIME_DATETIME_ONLY Set if we only allow full datetimes.
    was_cut             Set to 1 if value was cut during conversion or to 0
                        otherwise.

  DESCRIPTION
    At least the following formats are recogniced (based on number of digits)
    YYMMDD, YYYYMMDD, YYMMDDHHMMSS, YYYYMMDDHHMMSS
    YY-MM-DD, YYYY-MM-DD, YY-MM-DD HH.MM.SS
    YYYYMMDDTHHMMSS  where T is a the character T (ISO8601)
    Also dates where all parts are zero are allowed

    The second part may have an optional .###### fraction part.

  NOTES
   This function should work with a format position vector as long as the
   following things holds:
   - All date are kept together and all time parts are kept together
   - Date and time parts must be separated by blank
   - Second fractions must come after second part and be separated
     by a '.'.  (The second fractions are optional)
   - AM/PM must come after second fractions (or after seconds if no fractions)
   - Year must always been specified.
   - If time is before date, then we will use datetime format only if
     the argument consist of two parts, separated by space.
     Otherwise we will assume the argument is a date.
   - The hour part must be specified in hour-minute-second order.

  RETURN VALUES
    MYSQL_TIMESTAMP_NONE        String wasn't a timestamp, like
                                [DD [HH:[MM:[SS]]]].fraction.
                                l_time is not changed.
    MYSQL_TIMESTAMP_DATE        DATE string (YY MM and DD parts ok)
    MYSQL_TIMESTAMP_DATETIME    Full timestamp
    MYSQL_TIMESTAMP_ERROR       Timestamp with wrong values.
                                All elements in l_time is set to 0
*/

#define MAX_DATE_PARTS 8

enum enum_mysql_timestamp_type
str_to_datetime(const char *str, uint length, MYSQL_TIME *l_time,
                uint flags, int *was_cut)
{
  uint field_length, year_length, digits, i, number_of_fields;
  uint date[MAX_DATE_PARTS], date_len[MAX_DATE_PARTS];
  uint add_hours= 0, start_loop;
  ulong not_zero_date, allow_space;
  bool is_internal_format;
  const char *pos, *last_field_pos;
  const char *end=str+length;
  const uchar *format_position;
  bool found_delimitier= 0, found_space= 0;
  uint frac_pos, frac_len;
  DBUG_ENTER("str_to_datetime");
  DBUG_PRINT("ENTER",("str: %.*s",length,str));

  LINT_INIT(field_length);
  LINT_INIT(year_length);
  LINT_INIT(last_field_pos);

  *was_cut= 0;

  /* Skip space at start */
  for (; str != end && my_isspace(&my_charset_latin1, *str) ; str++)
    ;
  if (str == end || ! my_isdigit(&my_charset_latin1, *str))
  {
    *was_cut= 1;
    DBUG_RETURN(MYSQL_TIMESTAMP_NONE);
  }

  is_internal_format= 0;
  /* This has to be changed if want to activate different timestamp formats */
  format_position= internal_format_positions;

  /*
    Calculate number of digits in first part.
    If length= 8 or >= 14 then year is of format YYYY.
    (YYYY-MM-DD,  YYYYMMDD, YYYYYMMDDHHMMSS)
  */
  for (pos=str; pos != end && my_isdigit(&my_charset_latin1,*pos) ; pos++)
    ;

  digits= (uint) (pos-str);
  start_loop= 0;                                /* Start of scan loop */
  date_len[format_position[0]]= 0;              /* Length of year field */
  if (pos == end)
  {
    /* Found date in internal format (only numbers like YYYYMMDD) */
    year_length= (digits == 4 || digits == 8 || digits >= 14) ? 4 : 2;
    field_length=year_length-1;
    is_internal_format= 1;
    format_position= internal_format_positions;
  }
  else
  {
    if (format_position[0] >= 3)                /* If year is after HHMMDD */
    {
      /*
        If year is not in first part then we have to determinate if we got
        a date field or a datetime field.
        We do this by checking if there is two numbers separated by
        space in the input.
      */
      while (pos < end && !my_isspace(&my_charset_latin1, *pos))
        pos++;
      while (pos < end && !my_isdigit(&my_charset_latin1, *pos))
        pos++;
      if (pos == end)
      {
        if (flags & TIME_DATETIME_ONLY)
        {
          *was_cut= 1;
          DBUG_RETURN(MYSQL_TIMESTAMP_NONE);   /* Can't be a full datetime */
        }
        /* Date field.  Set hour, minutes and seconds to 0 */
        date[0]= date[1]= date[2]= date[3]= date[4]= 0;
        start_loop= 5;                         /* Start with first date part */
      }
    }
  }

  /*
    Only allow space in the first "part" of the datetime field and:
    - after days, part seconds
    - before and after AM/PM (handled by code later)

    2003-03-03 20:00:20 AM
    20:00:20.000000 AM 03-03-2000
  */
  i= max((uint) format_position[0], (uint) format_position[1]);
  set_if_bigger(i, (uint) format_position[2]);
  allow_space= ((1 << i) | (1 << format_position[6]));
  allow_space&= (1 | 2 | 4 | 8);

  not_zero_date= 0;
  for (i = start_loop;
       i < MAX_DATE_PARTS-1 && str != end &&
         my_isdigit(&my_charset_latin1,*str);
       i++)
  {
    const char *start= str;
    ulong tmp_value= (uint) (uchar) (*str++ - '0');
    while (str != end && my_isdigit(&my_charset_latin1,str[0]) &&
           (!is_internal_format || field_length--))
    {
      tmp_value=tmp_value*10 + (ulong) (uchar) (*str - '0');
      str++;
    }
    date_len[i]= (uint) (str - start);
    if (tmp_value > 999999)                     /* Impossible date part */
    {
      *was_cut= 1;
      DBUG_RETURN(MYSQL_TIMESTAMP_NONE);
    }
    date[i]=tmp_value;
    not_zero_date|= tmp_value;

    /* Length-1 of next field */
    field_length= format_position[i+1] == 0 ? 3 : 1;

    if ((last_field_pos= str) == end)
    {
      i++;                                      /* Register last found part */
      break;
    }
    /* Allow a 'T' after day to allow CCYYMMDDT type of fields */
    if (i == format_position[2] && *str == 'T')
    {
      str++;                                    /* ISO8601:  CCYYMMDDThhmmss */
      continue;
    }
    if (i == format_position[5])                /* Seconds */
    {
      if (*str == '.')                          /* Followed by part seconds */
      {
        str++;
        field_length= 5;                        /* 5 digits after first (=6) */
      }
      continue;

      /* No part seconds */
      date[++i]= 0;
    }
    while (str != end &&
           (my_ispunct(&my_charset_latin1,*str) ||
            my_isspace(&my_charset_latin1,*str)))
    {
      if (my_isspace(&my_charset_latin1,*str))
      {
        if (!(allow_space & (1 << i)))
        {
          *was_cut= 1;
          DBUG_RETURN(MYSQL_TIMESTAMP_NONE);
        }
        found_space= 1;
      }
      str++;
      found_delimitier= 1;                      /* Should be a 'normal' date */
    }
    /* Check if next position is AM/PM */
    if (i == format_position[6])                /* Seconds, time for AM/PM */
    {
      i++;                                      /* Skip AM/PM part */
      if (format_position[7] != 255)            /* If using AM/PM */
      {
        if (str+2 <= end && (str[1] == 'M' || str[1] == 'm'))
        {
          if (str[0] == 'p' || str[0] == 'P')
            add_hours= 12;
          else if (str[0] != 'a' || str[0] != 'A')
            continue;                           /* Not AM/PM */
          str+= 2;                              /* Skip AM/PM */
          /* Skip space after AM/PM */
          while (str != end && my_isspace(&my_charset_latin1,*str))
            str++;
        }
      }
    }
    last_field_pos= str;
  }
  if (found_delimitier && !found_space && (flags & TIME_DATETIME_ONLY))
  {
    *was_cut= 1;
    DBUG_RETURN(MYSQL_TIMESTAMP_NONE);          /* Can't be a datetime */
  }

  str= last_field_pos;

  number_of_fields= i - start_loop;
  while (i < MAX_DATE_PARTS)
  {
    date_len[i]= 0;
    date[i++]= 0;
  }

  if (!is_internal_format)
  {
    year_length= date_len[(uint) format_position[0]];
    if (!year_length)                           /* Year must be specified */
    {
      *was_cut= 1;
      DBUG_RETURN(MYSQL_TIMESTAMP_NONE);
    }

    l_time->year=               date[(uint) format_position[0]];
    l_time->month=              date[(uint) format_position[1]];
    l_time->day=                date[(uint) format_position[2]];
    l_time->hour=               date[(uint) format_position[3]];
    l_time->minute=             date[(uint) format_position[4]];
    l_time->second=             date[(uint) format_position[5]];

    frac_pos= (uint) format_position[6];
    frac_len= date_len[frac_pos];
    if (frac_len < 6)
      date[frac_pos]*= (uint) log_10_int[6 - frac_len];
    l_time->second_part= date[frac_pos];

    if (format_position[7] != (uchar) 255)
    {
      if (l_time->hour > 12)
      {
        *was_cut= 1;
        goto err;
      }
      l_time->hour= l_time->hour%12 + add_hours;
    }
  }
  else
  {
    l_time->year=       date[0];
    l_time->month=      date[1];
    l_time->day=        date[2];
    l_time->hour=       date[3];
    l_time->minute=     date[4];
    l_time->second=     date[5];
    if (date_len[6] < 6)
      date[6]*= (uint) log_10_int[6 - date_len[6]];
    l_time->second_part=date[6];
  }
  l_time->neg= 0;

  if (year_length == 2 && i >= format_position[1] && i >=format_position[2] &&
      (l_time->month || l_time->day))
    l_time->year+= (l_time->year < YY_PART_YEAR ? 2000 : 1900);

  if (number_of_fields < 3 || l_time->month > 12 ||
      l_time->day > 31 || l_time->hour > 23 ||
      l_time->minute > 59 || l_time->second > 59 ||
      (!(flags & TIME_FUZZY_DATE) && (l_time->month == 0 || l_time->day == 0)))
  {
    /* Only give warning for a zero date if there is some garbage after */
    if (!not_zero_date)                         /* If zero date */
    {
      for (; str != end ; str++)
      {
        if (!my_isspace(&my_charset_latin1, *str))
        {
          not_zero_date= 1;                     /* Give warning */
          break;
        }
      }
    }
    if (not_zero_date)
      *was_cut= 1;
    goto err;
  }

  l_time->time_type= (number_of_fields <= 3 ?
                      MYSQL_TIMESTAMP_DATE : MYSQL_TIMESTAMP_DATETIME);

  for (; str != end ; str++)
  {
    if (!my_isspace(&my_charset_latin1,*str))
    {
      *was_cut= 1;
      break;
    }
  }

  DBUG_RETURN(l_time->time_type=
              (number_of_fields <= 3 ? MYSQL_TIMESTAMP_DATE :
                                       MYSQL_TIMESTAMP_DATETIME));

err:
  bzero((char*) l_time, sizeof(*l_time));
  DBUG_RETURN(MYSQL_TIMESTAMP_ERROR);
}


/*
 Convert a time string to a TIME struct.

  SYNOPSIS
   str_to_time()
   str                  A string in full TIMESTAMP format or
                        [-] DAYS [H]H:MM:SS, [H]H:MM:SS, [M]M:SS, [H]HMMSS,
                        [M]MSS or [S]S
                        There may be an optional [.second_part] after seconds
   length               Length of str
   l_time               Store result here
   was_cut              Set to 1 if value was cut during conversion or to 0
                        otherwise.

   NOTES
     Because of the extra days argument, this function can only
     work with times where the time arguments are in the above order.

   RETURN
     0  ok
     1  error
*/

bool str_to_time(const char *str, uint length, MYSQL_TIME *l_time,
                 int *was_cut)
{
  long date[5],value;
  const char *end=str+length, *end_of_days;
  bool found_days,found_hours;
  uint state;

  l_time->neg=0;
  *was_cut= 0;
  for (; str != end && my_isspace(&my_charset_latin1,*str) ; str++)
    length--;
  if (str != end && *str == '-')
  {
    l_time->neg=1;
    str++;
    length--;
  }
  if (str == end)
    return 1;

  /* Check first if this is a full TIMESTAMP */
  if (length >= 12)
  {                                             /* Probably full timestamp */
    enum enum_mysql_timestamp_type
      res= str_to_datetime(str, length, l_time,
                           (TIME_FUZZY_DATE | TIME_DATETIME_ONLY), was_cut);
    if ((int) res >= (int) MYSQL_TIMESTAMP_ERROR)
      return res == MYSQL_TIMESTAMP_ERROR;
    /* We need to restore was_cut flag since str_to_datetime can modify it */
    *was_cut= 0;
  }

  /* Not a timestamp. Try to get this as a DAYS_TO_SECOND string */
  for (value=0; str != end && my_isdigit(&my_charset_latin1,*str) ; str++)
    value=value*10L + (long) (*str - '0');

  /* Skip all space after 'days' */
  end_of_days= str;
  for (; str != end && my_isspace(&my_charset_latin1, str[0]) ; str++)
    ;

  LINT_INIT(state);
  found_days=found_hours=0;
  if ((uint) (end-str) > 1 && str != end_of_days &&
      my_isdigit(&my_charset_latin1, *str))
  {                                             /* Found days part */
    date[0]= value;
    state= 1;                                   /* Assume next is hours */
    found_days= 1;
  }
  else if ((end-str) > 1 &&  *str == time_separator &&
           my_isdigit(&my_charset_latin1, str[1]))
  {
    date[0]=0;                                  /* Assume we found hours */
    date[1]=value;
    state=2;
    found_hours=1;
    str++;                                      /* skip ':' */
  }
  else
  {
    /* String given as one number; assume HHMMSS format */
    date[0]= 0;
    date[1]= value/10000;
    date[2]= value/100 % 100;
    date[3]= value % 100;
    state=4;
    goto fractional;
  }

  /* Read hours, minutes and seconds */
  for (;;)
  {
    for (value=0; str != end && my_isdigit(&my_charset_latin1,*str) ; str++)
      value=value*10L + (long) (*str - '0');
    date[state++]=value;
    if (state == 4 || (end-str) < 2 || *str != time_separator ||
        !my_isdigit(&my_charset_latin1,str[1]))
      break;
    str++;                                      /* Skip time_separator (':') */
  }

  if (state != 4)
  {                                             /* Not HH:MM:SS */
    /* Fix the date to assume that seconds was given */
    if (!found_hours && !found_days)
    {
      bmove_upp((char*) (date+4), (char*) (date+state),
                sizeof(long)*(state-1));
      bzero((char*) date, sizeof(long)*(4-state));
    }
    else
      bzero((char*) (date+state), sizeof(long)*(4-state));
  }

fractional:
  /* Get fractional second part */
  if ((end-str) >= 2 && *str == '.' && my_isdigit(&my_charset_latin1,str[1]))
  {
    uint field_length=5;
    str++; value=(uint) (uchar) (*str - '0');
    while (++str != end && 
           my_isdigit(&my_charset_latin1,str[0]) && 
           field_length--)
      value=value*10 + (uint) (uchar) (*str - '0');
    if (field_length)
      value*= (long) log_10_int[field_length];
    date[4]=value;
  }
  else
    date[4]=0;

  if (internal_format_positions[7] != 255)
  {
    /* Read a possible AM/PM */
    while (str != end && my_isspace(&my_charset_latin1, *str))
      str++;
    if (str+2 <= end && (str[1] == 'M' || str[1] == 'm'))
    {
      if (str[0] == 'p' || str[0] == 'P')
      {
        str+= 2;
        date[1]= date[1]%12 + 12;
      }
      else if (str[0] == 'a' || str[0] == 'A')
        str+=2;
    }
  }

  /* Some simple checks */
  if (date[2] >= 60 || date[3] >= 60)
  {
    *was_cut= 1;
    return 1;
  }
  l_time->year=         0;                      /* For protocol::store_time */
  l_time->month=        0;
  l_time->day=          date[0];
  l_time->hour=         date[1];
  l_time->minute=       date[2];
  l_time->second=       date[3];
  l_time->second_part=  date[4];
  l_time->time_type= MYSQL_TIMESTAMP_TIME;

  /* Check if there is garbage at end of the TIME specification */
  if (str != end)
  {
    do
    {
      if (!my_isspace(&my_charset_latin1,*str))
      {
        *was_cut= 1;
        break;
      }
    } while (++str != end);
  }
  return 0;
}

