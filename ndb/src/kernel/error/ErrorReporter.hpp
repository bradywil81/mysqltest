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

#ifndef ERRORREPORTER_H
#define ERRORREPORTER_H

#include <ndb_global.h>

#include "TimeModule.hpp"
#include "Error.hpp"
#include <Emulator.hpp>


#ifdef ASSERT
#undef ASSERT
#endif

#define REQUIRE(trueToContinue, message) \
    if ( (trueToContinue) ) { } else { \
          ErrorReporter::handleAssert(message, __FILE__, __LINE__); }

#define THREAD_REQUIRE(trueToContinue, message) \
    if ( (trueToContinue) ) { } else { \
          ErrorReporter::handleThreadAssert(message, __FILE__, __LINE__); }

#ifdef NDEBUG

#define NDB_ASSERT(trueToContinue, message)
#define THREAD_ASSERT(trueToContinue, message)

#else

#define NDB_ASSERT(trueToContinue, message) \
    if ( !(trueToContinue) ) { \
          ErrorReporter::handleAssert(message, __FILE__, __LINE__); }

#define THREAD_ASSERT(trueToContinue, message) \
    if ( !(trueToContinue) ) { \
          ErrorReporter::handleThreadAssert(message, __FILE__, __LINE__); }

#endif
        // Description:
        //      This macro is used to report programming errors.
        // Parameters:
        //      trueToContinue  IN      An expression. If it evaluates to 0
        //                              execution is stopped.
        //      message         IN      A message from the programmer 
        //                              explaining what went wrong.

class ErrorReporter
{
public:
  static void handleAssert(const char* message, 
			   const char* file, 
			   int line);
  
  static void handleThreadAssert(const char* message, 
     		                 const char* file, 
		                 int line);
  
  static void handleError(ErrorCategory type, 
			  int faultID, 
			  const char* problemData,
                          const char* objRef,
			  enum NdbShutdownType = NST_ErrorHandler);
  
  static void handleWarning(ErrorCategory type, 
			    int faultID, 
			    const char* problemData,
                            const char* objRef);
  
  static void formatMessage(ErrorCategory type, 
			    int faultID, 
			    const char* problemData,
                            const char* objRef, 
			    const char* theNameOfTheTraceFile,
			    char* messptr);

  static void formatTraceFileName(char* theName, int maxLen);
  
  static const char* formatTimeStampString();
  
private:
};

#endif