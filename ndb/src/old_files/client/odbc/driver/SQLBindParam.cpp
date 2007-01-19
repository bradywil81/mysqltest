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

#include "driver.hpp"

#if ODBCVER >= 0x0300
SQLRETURN SQL_API
SQLBindParam(
    SQLHSTMT StatementHandle,
    SQLUSMALLINT ParameterNumber,
    SQLSMALLINT ValueType,
    SQLSMALLINT ParameterType,
    SQLUINTEGER LengthPrecision,
    SQLSMALLINT ParameterScale,
    SQLPOINTER ParameterValue,
    SQLINTEGER* StrLen_or_Ind)
{
    driver_enter(SQL_API_SQLBINDPARAM);
    const char* const sqlFunction = "SQLBindParam";
    HandleRoot* const pRoot = HandleRoot::instance();
    HandleStmt* pStmt = pRoot->findStmt(StatementHandle);
    if (pStmt == 0) {
	driver_exit(SQL_API_SQLBINDPARAM);
        return SQL_INVALID_HANDLE;
    }
    Ctx& ctx = *new Ctx;
    ctx.logSqlEnter(sqlFunction);
    try {
	pStmt->sqlBindParam(ctx, ParameterNumber, ValueType, ParameterType, LengthPrecision, ParameterScale, ParameterValue, StrLen_or_Ind);
    } catch (CtxAssert& ctxAssert) {
	ctx.handleEx(ctxAssert);
    }
    pStmt->saveCtx(ctx);
    ctx.logSqlExit();
    SQLRETURN ret = ctx.getCode();
    driver_exit(SQL_API_SQLBINDPARAM);
    return ret;
}
#endif // ODBCVER >= 0x0300