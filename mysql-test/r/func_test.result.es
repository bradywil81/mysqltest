drop table if exists t1,t2;
select 0=0,1>0,1>=1,1<0,1<=0,1!=0,strcmp("abc","abcd"),strcmp("b","a"),strcmp("a","a") ;
0=0	1>0	1>=1	1<0	1<=0	1!=0	strcmp("abc","abcd")	strcmp("b","a")	strcmp("a","a")
1	1	1	0	0	1	-1	1	0
select "a"<"b","a"<="b","b">="a","b">"a","a"="A","a"<>"b";
"a"<"b"	"a"<="b"	"b">="a"	"b">"a"	"a"="A"	"a"<>"b"
1	1	1	1	1	1
select "a "="A", "A "="a", "a  " <= "A b";
"a "="A"	"A "="a"	"a  " <= "A b"
1	1	1
select "abc" like "a%", "abc" not like "%d%", "a%" like "a\%","abc%" like "a%\%","abcd" like "a%b_%d", "a" like "%%a","abcde" like "a%_e","abc" like "abc%";
"abc" like "a%"	"abc" not like "%d%"	"a%" like "a\%"	"abc%" like "a%\%"	"abcd" like "a%b_%d"	"a" like "%%a"	"abcde" like "a%_e"	"abc" like "abc%"
1	1	1	1	1	1	1	1
select "a" like "%%b","a" like "%%ab","ab" like "a\%", "ab" like "_", "ab" like "ab_", "abc" like "%_d", "abc" like "abc%d";
"a" like "%%b"	"a" like "%%ab"	"ab" like "a\%"	"ab" like "_"	"ab" like "ab_"	"abc" like "%_d"	"abc" like "abc%d"
0	0	0	0	0	0	0
select '?' like '|%', '?' like '|%' ESCAPE '|', '%' like '|%', '%' like '|%' ESCAPE '|', '%' like '%';
'?' like '|%'	'?' like '|%' ESCAPE '|'	'%' like '|%'	'%' like '|%' ESCAPE '|'	'%' like '%'
0	0	0	1	1
select 'abc' like '%c','abcabc' like '%c',  "ab" like "", "ab" like "a", "ab" like "ab";
'abc' like '%c'	'abcabc' like '%c'	"ab" like ""	"ab" like "a"	"ab" like "ab"
1	1	0	0	1
select "Det h�r �r svenska" regexp "h[[:alpha:]]+r", "aba" regexp "^(a|b)*$";
"Det här är svenska" regexp "h[[:alpha:]]+r"	"aba" regexp "^(a|b)*$"
1	1
select "aba" regexp concat("^","a");
"aba" regexp concat("^","a")
1
select !0,NOT 0=1,!(0=0),1 AND 1,1 && 0,0 OR 1,1 || NULL, 1=1 or 1=1 and 1=0;
!0	NOT 0=1	!(0=0)	1 AND 1	1 && 0	0 OR 1	1 || NULL	1=1 or 1=1 and 1=0
1	1	0	1	0	1	1	1
select 2 between 1 and 3, "monty" between "max" and "my",2=2 and "monty" between "max" and "my" and 3=3;
2 between 1 and 3	"monty" between "max" and "my"	2=2 and "monty" between "max" and "my" and 3=3
1	1	1
select 'b' between 'a' and 'c', 'B' between 'a' and 'c';
'b' between 'a' and 'c'	'B' between 'a' and 'c'
1	1
select 2 in (3,2,5,9,5,1),"monty" in ("david","monty","allan"), 1.2 in (1.4,1.2,1.0);
2 in (3,2,5,9,5,1)	"monty" in ("david","monty","allan")	1.2 in (1.4,1.2,1.0)
1	1	1
select -1.49 or -1.49,0.6 or 0.6;
-1.49 or -1.49	0.6 or 0.6
1	1
select 3 ^ 11, 1 ^ 1, 1 ^ 0, 1 ^ NULL, NULL ^ 1;
3 ^ 11	1 ^ 1	1 ^ 0	1 ^ NULL	NULL ^ 1
8	0	1	NULL	NULL
explain extended select 3 ^ 11, 1 ^ 1, 1 ^ 0, 1 ^ NULL, NULL ^ 1;
id	select_type	table	type	possible_keys	key	key_len	ref	rows	Extra
1	SIMPLE	NULL	NULL	NULL	NULL	NULL	NULL	NULL	No tables used
Warnings:
Note	1003	select (3 ^ 11) AS `3 ^ 11`,(1 ^ 1) AS `1 ^ 1`,(1 ^ 0) AS `1 ^ 0`,(1 ^ NULL) AS `1 ^ NULL`,(NULL ^ 1) AS `NULL ^ 1`
select 1 XOR 1, 1 XOR 0, 0 XOR 1, 0 XOR 0, NULL XOR 1, 1 XOR NULL, 0 XOR NULL;
1 XOR 1	1 XOR 0	0 XOR 1	0 XOR 0	NULL XOR 1	1 XOR NULL	0 XOR NULL
0	1	1	0	NULL	NULL	NULL
select 1 like 2 xor 2 like 1;
1 like 2 xor 2 like 1
0
select 10 % 7, 10 mod 7, 10 div 3;
10 % 7	10 mod 7	10 div 3
3	3	3
explain extended select 10 % 7, 10 mod 7, 10 div 3;
id	select_type	table	type	possible_keys	key	key_len	ref	rows	Extra
1	SIMPLE	NULL	NULL	NULL	NULL	NULL	NULL	NULL	No tables used
Warnings:
Note	1003	select (10 % 7) AS `10 % 7`,(10 % 7) AS `10 mod 7`,(10 DIV 3) AS `10 div 3`
select (1 << 64)-1, ((1 << 64)-1) DIV 1, ((1 << 64)-1) DIV 2;
(1 << 64)-1	((1 << 64)-1) DIV 1	((1 << 64)-1) DIV 2
18446744073709551615	18446744073709551615	9223372036854775807
explain extended select (1 << 64)-1, ((1 << 64)-1) DIV 1, ((1 << 64)-1) DIV 2;
id	select_type	table	type	possible_keys	key	key_len	ref	rows	Extra
1	SIMPLE	NULL	NULL	NULL	NULL	NULL	NULL	NULL	No tables used
Warnings:
Note	1003	select ((1 << 64) - 1) AS `(1 << 64)-1`,(((1 << 64) - 1) DIV 1) AS `((1 << 64)-1) DIV 1`,(((1 << 64) - 1) DIV 2) AS `((1 << 64)-1) DIV 2`
create table t1 (a int);
insert t1 values (1);
select * from t1 where 1 xor 1;
a
explain extended select * from t1 where 1 xor 1;
id	select_type	table	type	possible_keys	key	key_len	ref	rows	Extra
1	SIMPLE	NULL	NULL	NULL	NULL	NULL	NULL	NULL	Impossible WHERE noticed after reading const tables
Warnings:
Note	1003	select test.t1.a AS `a` from test.t1 where (1 xor 1)
select - a from t1;
- a
-1
explain extended select - a from t1;
id	select_type	table	type	possible_keys	key	key_len	ref	rows	Extra
1	SIMPLE	t1	system	NULL	NULL	NULL	NULL	1	
Warnings:
Note	1003	select -(test.t1.a) AS `- a` from test.t1
drop table t1;
select 5 between 0 and 10 between 0 and 1,(5 between 0 and 10) between 0 and 1;
5 between 0 and 10 between 0 and 1	(5 between 0 and 10) between 0 and 1
0	1
select 1 and 2 between 2 and 10, 2 between 2 and 10 and 1;
1 and 2 between 2 and 10	2 between 2 and 10 and 1
1	1
select 1 and 0 or 2, 2 or 1 and 0;
1 and 0 or 2	2 or 1 and 0
1	1
select _koi8r'a' = _koi8r'A';
_koi8r'a' = _koi8r'A'
1
select _koi8r'a' = _koi8r'A' COLLATE koi8r_general_ci;
_koi8r'a' = _koi8r'A' COLLATE koi8r_general_ci
1
explain extended select _koi8r'a' = _koi8r'A' COLLATE koi8r_general_ci;
id	select_type	table	type	possible_keys	key	key_len	ref	rows	Extra
1	SIMPLE	NULL	NULL	NULL	NULL	NULL	NULL	NULL	No tables used
Warnings:
Note	1003	select (_koi8r'a' = (_koi8r'A' collate _latin1'koi8r_general_ci')) AS `_koi8r'a' = _koi8r'A' COLLATE koi8r_general_ci`
select _koi8r'a' = _koi8r'A' COLLATE koi8r_bin;
_koi8r'a' = _koi8r'A' COLLATE koi8r_bin
0
select _koi8r'a' COLLATE koi8r_general_ci = _koi8r'A';
_koi8r'a' COLLATE koi8r_general_ci = _koi8r'A'
1
select _koi8r'a' COLLATE koi8r_bin = _koi8r'A';
_koi8r'a' COLLATE koi8r_bin = _koi8r'A'
0
select _koi8r'a' COLLATE koi8r_bin = _koi8r'A' COLLATE koi8r_general_ci;
ERROR HY000: Illegal mix of collations (koi8r_bin,EXPLICIT) and (koi8r_general_ci,EXPLICIT) for operation '='
select _koi8r'a' = _latin1'A';
ERROR HY000: Illegal mix of collations (koi8r_general_ci,COERCIBLE) and (latin1_swedish_ci,COERCIBLE) for operation '='
select strcmp(_koi8r'a', _koi8r'A');
strcmp(_koi8r'a', _koi8r'A')
0
select strcmp(_koi8r'a', _koi8r'A' COLLATE koi8r_general_ci);
strcmp(_koi8r'a', _koi8r'A' COLLATE koi8r_general_ci)
0
select strcmp(_koi8r'a', _koi8r'A' COLLATE koi8r_bin);
strcmp(_koi8r'a', _koi8r'A' COLLATE koi8r_bin)
1
select strcmp(_koi8r'a' COLLATE koi8r_general_ci, _koi8r'A');
strcmp(_koi8r'a' COLLATE koi8r_general_ci, _koi8r'A')
0
select strcmp(_koi8r'a' COLLATE koi8r_bin, _koi8r'A');
strcmp(_koi8r'a' COLLATE koi8r_bin, _koi8r'A')
1
select strcmp(_koi8r'a' COLLATE koi8r_general_ci, _koi8r'A' COLLATE koi8r_bin);
ERROR HY000: Illegal mix of collations (koi8r_general_ci,EXPLICIT) and (koi8r_bin,EXPLICIT) for operation 'strcmp'
select strcmp(_koi8r'a', _latin1'A');
ERROR HY000: Illegal mix of collations (koi8r_general_ci,COERCIBLE) and (latin1_swedish_ci,COERCIBLE) for operation 'strcmp'
select _koi8r'a' LIKE _koi8r'A';
_koi8r'a' LIKE _koi8r'A'
1
select _koi8r'a' LIKE _koi8r'A' COLLATE koi8r_general_ci;
_koi8r'a' LIKE _koi8r'A' COLLATE koi8r_general_ci
1
select _koi8r'a' LIKE _koi8r'A' COLLATE koi8r_bin;
_koi8r'a' LIKE _koi8r'A' COLLATE koi8r_bin
0
select _koi8r'a' COLLATE koi8r_general_ci LIKE _koi8r'A';
_koi8r'a' COLLATE koi8r_general_ci LIKE _koi8r'A'
1
select _koi8r'a' COLLATE koi8r_bin LIKE _koi8r'A';
_koi8r'a' COLLATE koi8r_bin LIKE _koi8r'A'
0
select _koi8r'a' COLLATE koi8r_general_ci LIKE _koi8r'A' COLLATE koi8r_bin;
ERROR HY000: Illegal mix of collations (koi8r_general_ci,EXPLICIT) and (koi8r_bin,EXPLICIT) for operation 'like'
select _koi8r'a' LIKE _latin1'A';
ERROR HY000: Illegal mix of collations (koi8r_general_ci,COERCIBLE) and (latin1_swedish_ci,COERCIBLE) for operation 'like'
CREATE TABLE t1 (   faq_group_id int(11) NOT NULL default '0',   faq_id int(11) NOT NULL default '0',   title varchar(240) default NULL,   keywords text,   description longblob,   solution longblob,   status tinyint(4) NOT NULL default '0',   access_id smallint(6) default NULL,   lang_id smallint(6) NOT NULL default '0',   created datetime NOT NULL default '0000-00-00 00:00:00',   updated datetime default NULL,   last_access datetime default NULL,   last_notify datetime default NULL,   solved_count int(11) NOT NULL default '0',   static_solved int(11) default NULL,   solved_1 int(11) default NULL,   solved_2 int(11) default NULL,   solved_3 int(11) default NULL,   solved_4 int(11) default NULL,   solved_5 int(11) default NULL,   expires datetime default NULL,   notes text,   assigned_to smallint(6) default NULL,   assigned_group smallint(6) default NULL,   last_edited_by smallint(6) default NULL,   orig_ref_no varchar(15) binary default NULL,   c$fundstate smallint(6) default NULL,   c$contributor smallint(6) default NULL,   UNIQUE KEY t1$faq_id (faq_id),   KEY t1$group_id$faq_id (faq_group_id,faq_id),   KEY t1$c$fundstate (c$fundstate) ) ENGINE=MyISAM;
INSERT INTO t1 VALUES (82,82,'How to use the DynaVox Usage Counts Feature','usages count, number, corner, white, box, button','<as-html>\r\n<table width=\"100%\" border=\"0\">\r\n  <tr>\r\n    <td width=\"3%\">�</td>\r\n    <td width=\"97%\">\r\n       <h3><font face=\"Verdana, Arial, Helvetica, sans-serif\" color=\"#000000\">How \r\n        To</font><!-- #BeginEditable \"CS_troubleshoot_question\" --><font face=\"Verdana, Arial, Helvetica, sans-serif\" color=\"#000099\"><font color=\"#000000\">: \r\n        Display or Hide the Usage Counts to find out how many times each button is being selected. </font></font><!-- #EndEditable --></h3>\r\n    </td>\r\n  </tr>\r\n</table>','<as-html>\r\n <table width=\"100%\" border=\"0\">\r\n  <tr>\r\n    <td width=\"3%\">�</td>\r\n    \r\n<td width=\"97%\"><!-- #BeginEditable \"CS_troubleshoot_answer\" --> \r\n      \r\n<p><font color=\"#000000\" face=\"Verdana, Arial, Helvetica, sans-serif\">1. Select \r\n  the <i>On/Setup</i> button to access the DynaVox Setup Menu.<br>\r\n  2. Select <b>Button Features.</b><br>\r\n  3. Below the <b>OK</b> button is the <b>Usage Counts</b> button.<br>\r\n  a. If it says \"Hidden\" then the Usage Counts will not be displayed.<br>\r\n  b. If it says \"Displayed\" then the Usage Counts will be shown.<br>\r\n        c. Select the <b>Usage Counts</b> Option Ring once and it will toggle \r\n        to the alternative option.<br>\r\n  4. Once the correct setting has been chosen, select <b>OK</b> to leave the <i>Button \r\n  Features</i> menu.<br>\r\n  5. Select <b>OK</b> out of the <i>Setup</i> menu and return to the communication \r\n  page.</font></p>\r\n      <p><font color=\"#000000\" face=\"Verdana, Arial, Helvetica, sans-serif\">For \r\n        further information on <i>Usage Counts,</i> see the <i>Button Features \r\n        Menu Entry</i> in the DynaVox/DynaMyte Reference Manual.</font></p>\r\n<!-- #EndEditable --></td>\r\n  </tr>\r\n</table>',4,1,1,'2001-11-16 16:43:34','2002-11-25 12:09:43','2003-07-24 01:04:48',NULL,11,NULL,0,0,0,0,0,NULL,NULL,NULL,NULL,11,NULL,NULL,NULL);
CREATE TABLE t2 (  access_id smallint(6) NOT NULL default '0',   name varchar(20) binary default NULL,   rank smallint(6) NOT NULL default '0',   KEY t2$access_id (access_id) ) ENGINE=MyISAM;
INSERT INTO t2 VALUES (1,'Everyone',2),(2,'Help',3),(3,'Customer Support',1);
SELECT f_acc.rank, a1.rank, a2.rank  FROM t1 LEFT JOIN t1 f1 ON  (f1.access_id=1 AND f1.faq_group_id = t1.faq_group_id) LEFT JOIN t2 a1 ON (a1.access_id =  f1.access_id) LEFT JOIN t1 f2 ON (f2.access_id=3 AND  f2.faq_group_id = t1.faq_group_id) LEFT  JOIN t2 a2 ON (a2.access_id = f2.access_id), t2 f_acc WHERE LEAST(a1.rank,a2.rank) =  f_acc.rank;
rank	rank	rank
2	2	NULL
DROP TABLE t1,t2;
CREATE TABLE t1 (d varchar(6), k int);
INSERT INTO t1 VALUES (NULL, 2);
SELECT GREATEST(d,d) FROM t1 WHERE k=2;
GREATEST(d,d)
NULL
DROP TABLE t1;
select 1197.90 mod 50;
1197.90 mod 50
47.90
select 5.1 mod 3, 5.1 mod -3, -5.1 mod 3, -5.1 mod -3;
5.1 mod 3	5.1 mod -3	-5.1 mod 3	-5.1 mod -3
2.1	2.1	-2.1	-2.1
select 5 mod 3, 5 mod -3, -5 mod 3, -5 mod -3;
5 mod 3	5 mod -3	-5 mod 3	-5 mod -3
2	2	-2	-2