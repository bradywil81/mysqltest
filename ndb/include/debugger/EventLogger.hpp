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

#ifndef EVENTLOGGER_H
#define EVENTLOGGER_H

#include <Logger.hpp>
#include <FileLogHandler.hpp>
#include <GrepError.hpp>
#include <kernel_types.h>
#include <kernel/LogLevel.hpp>
#include <signaldata/EventReport.hpp>

/**
 * The EventLogger is primarily used for logging NDB events 
 * in the Management Server. It inherits all logging functionality of Logger.
 *
 * HOW TO USE
 *
 * 1) Create an EventLogger
 * 
 *   EventLogger myEventLogger = new EventLogger();
 * 
 * 2) Log NDB events and other log messages.
 *
 *   myEventLogger->info("Changing log levels.");
 *   
 *   EventReport* report = (EventReport*)&theSignalData[0];
 *   myEventLogger->log(eventReport->getEventType(), theSignalData, aNodeId);
 * 
 *
 * The following NDB event categories and log levels are enabled as default:
 *
 *  EVENT-CATEGORY LOG-LEVEL
 *
 *  Startup         4
 *  Shutdown        1
 *  Statistic       2 
 *  Checkpoint      5
 *  NodeRestart     8
 *  Connection      2
 *  Error          15 
 *  Info           10 
 *
 * @see Logger
 * @version #@ $Id: EventLogger.hpp,v 1.3 2003/09/01 10:15:52 innpeno Exp $
 */
class EventLogger : public Logger
{
public:
  /**
   * Default constructor. Enables default log levels and 
   * sets the log category to 'EventLogger'.
   */
  EventLogger();

  /**
   * Destructor.
   */
  ~EventLogger();

  /**
   * Open/create the eventlog, the default name is 'cluster.log'.
   *
   * @return true if successful.
   */
  bool open();

  /**
   * Opens/creates the eventlog with the specified filename.
   *
   * @param aFileName the eventlog filename.
   * @param maxNoFiles the maximum no of archived eventlog files.
   * @param maxFileSize the maximum eventlog file size.
   * @param maxLogEntries the maximum number of log entries before 
   *                      checking time to archive.
   * @return true if successful.
   */
  bool open(const char* logFileName,
	    int maxNoFiles = FileLogHandler::MAX_NO_FILES, 
	    long int maxFileSize = FileLogHandler::MAX_FILE_SIZE,
	    unsigned int maxLogEntries = FileLogHandler::MAX_LOG_ENTRIES);

  /**
   * Closes the eventlog.
   */
  void close();

  /**
   * Logs the NDB event.
   *
   * @param nodeId the node id of event origin.
   * @param eventType the type of event.
   * @param theData the event data.
   * @deprecated use log(int eventType, const Uint32* theData, NodeId nodeId)
   */
  void log(NodeId nodeId, int eventType, const Uint32* theData);

  /**
   * Logs the NDB event.
   *
   * @param eventType the type of event.
   * @param theData the event data.
   * @param nodeId the node id of event origin.
   */
  void log(int eventType, const Uint32* theData, NodeId nodeId = 0);

  /**
   * Returns the current log levels. 
   * Enable, disable log levels to filter the events that are sent to the
   * eventlog.
   *
   * @return the log level.
   */
  LogLevel& getLoglevel();
  
  /**
   * Returns the log level that is used to filter an event. The event will not
   * be logged unless its event category's log level is <= levelFilter.
   *
   * @return the log level filter that is used for all event categories.
   */
  int getFilterLevel() const;
  /**
   * Sets log level filter. The event will be logged if 
   * the event category's log level is <= 'filterLevel'.
   *
   * @param level the log level to filter.
   */
  void setFilterLevel(int filterLevel);

  /**
   * Returns the event text for the specified event report type.
   *
   * @param type the event type.
   * @param theData the event data.
   * @param nodeId a node id.
   * @return the event report text.
   */
  static const char* getText(int type,
			     const Uint32* theData, NodeId nodeId = 0);

  /**
   * Find a category matching the string
   *
   * @param str string to match.
   * @param cat the event category.
   * @param exactMatch only do exact matching.
   *
   *  @return TRUE if match is found, then cat is modified
   *          FALSE if match is not found
   */
  static bool matchEventCategory(const char * str, 
				 LogLevel::EventCategory * cat,
				 bool exactMatch = false);
  
  /**
   * Returns category name or NULL if not found.
   *
   * @param cat the event category.
   * @return category name.
   */
  static const char * getEventCategoryName(LogLevel::EventCategory cat);

  /**
   * Specifies allowed event categories/log levels.
   */
  struct EventCategoryName {
    LogLevel::EventCategory category;
    const char * name;
  };

  static const EventCategoryName eventCategoryNames[];
  static const Uint32 noOfEventCategoryNames;

  /**
   * This matrix defines which event should be printed when
   *
   * threshold - is in range [0-15]
   * severity  - DEBUG to ALERT (Type of log message)
   */  
  struct EventRepLogLevelMatrix {
    EventReport::EventType        eventType;
    LogLevel::EventCategory   eventCategory;
    Uint32                        threshold;
    Logger::LoggerLevel            severity;
  };

  static const EventRepLogLevelMatrix matrix[];

  /**
   * Default log levels for management nodes.
   *
   * threshold - is in range [0-15]
   */
  struct EventLogMatrix {
    LogLevel::EventCategory eventCategory;
    Uint32                  threshold;
  };

  static const EventLogMatrix defEventLogMatrix[];

  
  static const Uint32 matrixSize;
  static const Uint32 defEventLogMatrixSize;

private:
  /** Prohibit */
  EventLogger(const EventLogger&);
  EventLogger operator = (const EventLogger&);
  bool operator == (const EventLogger&);

  LogLevel m_logLevel;
  Uint32 m_filterLevel;

  STATIC_CONST(MAX_TEXT_LENGTH = 256);
  static char m_text[MAX_TEXT_LENGTH];
};


#endif