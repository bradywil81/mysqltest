/* Copyright (C) 2003 MySQL AB

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

#ifndef ConfigInfo_H
#define ConfigInfo_H

#include <kernel_types.h>
#include <Properties.hpp>
#include <ndb_limits.h>
#include <NdbOut.hpp>
#include "InitConfigFileParser.hpp"

/**
 * A MANDATORY parameters must be specified in the config file
 * An UNDEFINED parameter may or may not be specified in the config file
 */
static const Uint32 MANDATORY = ~0;     // Default value for mandatory params.
static const Uint32 UNDEFINED = (~0)-1; // Default value for undefined params.

/**
 * @class  ConfigInfo
 * @brief  Metainformation about ALL cluster configuration parameters 
 *
 * Use the getters to find out metainformation about parameters.
 */
class ConfigInfo {
public:
  enum Type        {BOOL, INT, STRING};
  enum Status      {USED,            ///< Active
		    DEPRICATED,      ///< Can be, but should not be used anymore
		    NOTIMPLEMENTED,  ///< Can not be used currently. Is ignored.
                    INTERNAL         ///< Not configurable by the user
  };

  /**
   *   Entry for one configuration parameter
   */
  struct ParamInfo {
    const char*    _fname;   
    const char*    _pname;
    const char*    _section;
    const char*    _description;
    Status         _status;
    bool           _updateable;    
    Type           _type;          
    Uint32         _default;
    Uint32         _min;
    Uint32         _max;
  };

  /**
   * Entry for one section rule
   */
  struct SectionRule {
    const char * m_section;
    bool (* m_sectionRule)(struct InitConfigFileParser::Context &, 
			   const char * m_ruleData);
    const char * m_ruleData;
  };
  
  ConfigInfo();

  /**
   *   Checks if the suggested value is valid for the suggested parameter
   *   (i.e. if it is >= than min and <= than max).
   *
   *   @param  section  Init Config file section name
   *   @param  fname    Name of parameter
   *   @param  value    Value to check
   *   @return true if parameter value is valid.
   * 
   *   @note Result is not defined if section/name are wrong!
   */
  bool verify(const Properties* section, const char* fname, Uint32 value) const;
  bool isSection(const char*) const;

  const char*  getPName(const Properties * section, const char* fname) const;
  const char*  getDescription(const Properties * section, const char* fname) const;
  Type         getType(const Properties * section, const char* fname) const;
  Status       getStatus(const Properties* section, const char* fname) const;
  Uint32       getMin(const Properties * section, const char* fname) const;
  Uint32       getMax(const Properties * section, const char* fname) const;
  Uint32       getDefault(const Properties * section, const char* fname) const;
  
  const Properties * getInfo(const char * section) const;
  const Properties * getDefaults(const char * section) const;
  
  void print() const;
  void print(const char* section) const;
  void print(const Properties * section, const char* parameter) const;

private:
  Properties               m_info;
  Properties               m_systemDefaults;

  static const ParamInfo   m_ParamInfo[];
  static const int         m_NoOfParams;
  
  static const char*       m_sectionNames[];
  static const int         m_noOfSectionNames;

public:
  static const SectionRule m_SectionRules[];
  static const int         m_NoOfRules;
};

#endif // ConfigInfo_H