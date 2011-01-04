/*
+-----------------------------------------------------------------------------------+
|  httpush                                                                          |
|  Copyright (c) 2010, Mikko Koppanen <mkoppanen@php.net>                           |
|  All rights reserved.                                                             |
+-----------------------------------------------------------------------------------+
|  Redistribution and use in source and binary forms, with or without               |
|  modification, are permitted provided that the following conditions are met:      |
|     * Redistributions of source code must retain the above copyright              |
|       notice, this list of conditions and the following disclaimer.               |
|     * Redistributions in binary form must reproduce the above copyright           |
|       notice, this list of conditions and the following disclaimer in the         |
|       documentation and/or other materials provided with the distribution.        |
|     * Neither the name of the copyright holder nor the                            |
|       names of its contributors may be used to endorse or promote products        |
|       derived from this software without specific prior written permission.       |
+-----------------------------------------------------------------------------------+
|  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND  |
|  ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED    |
|  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE           |
|  DISCLAIMED. IN NO EVENT SHALL MIKKO KOPPANEN BE LIABLE FOR ANY                   |
|  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES       |
|  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;     |
|  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND      |
|  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT       |
|  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS    |
|  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.                     |
+-----------------------------------------------------------------------------------+
*/

#ifndef __HP_LOG_H__
# define __HP_LOG_H__

#ifdef DEBUG
#define HP_DO_LOG(level_, ...) { \
    time_t my_time; \
    char buffer_[26], line_[256]; \
    time(&my_time); \
    ctime_r(&my_time, buffer_); \
    (void) snprintf(line_, 256, __VA_ARGS__); \
    fprintf(stderr, "[%24.24s] [%s:%d] [%s] %s\n", buffer_, __FILE__, __LINE__, level_, line_); \
}

#define HP_LOG_FATAL(...) HP_DO_LOG("FATAL", __VA_ARGS__);
#define HP_LOG_ERROR(...) HP_DO_LOG("ERROR", __VA_ARGS__);
#define HP_LOG_WARN(...)  HP_DO_LOG("WARN", __VA_ARGS__);
#define HP_LOG_INFO(...)  HP_DO_LOG("INFO", __VA_ARGS__);
#define HP_LOG_DEBUG(...) HP_DO_LOG("DEBUG", __VA_ARGS__);
#else
#define HP_DO_LOG(level_, ...) syslog(level_, __VA_ARGS__);
#define HP_LOG_FATAL(...)      HP_DO_LOG(LOG_EMERG, __VA_ARGS__);
#define HP_LOG_ERROR(...)      HP_DO_LOG(LOG_ERR, __VA_ARGS__);
#define HP_LOG_WARN(...)       HP_DO_LOG(LOG_WARNING, __VA_ARGS__);
#define HP_LOG_INFO(...)       HP_DO_LOG(LOG_INFO, __VA_ARGS__);
#define HP_LOG_DEBUG(...)
#endif

#endif /* __HP_LOG_H__ */
