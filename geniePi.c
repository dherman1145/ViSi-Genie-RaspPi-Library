/*
 * geniePi.c:
 *	Library to utilise the 4D Systems Genie interface to displays
 *	that have been created using the Visi-Genie creator platform.
 *	Primarily aimed at the Raspberry Pi, however this may be used
 *	on most Linux platforms with a serial connection (USB on
 *	on-board) to the 4D Systems Intelligent displays.
 *
 *	Gordon Henderson, December 2012, <projects@drogon.net>
 *	Copyright (c) 2012 4D Systems PTY Ltd, Sydney, Australia
 ***********************************************************************
 * This file is part of geniePi:
 *    geniePi is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as
 *    published by the Free Software Foundation, either version 3 of the
 *    License, or (at your option) any later version.
 *
 *    geniePi is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with geniePi.
 *    If not, see <http://www.gnu.org/licenses/>.
 ***********************************************************************
 */

#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <time.h>
#include <sys/time.h>

#include <sched.h>
#include <pthread.h>

#include "geniePi.h"

#ifndef	TRUE
#  define	TRUE (1==1)
#  define	FALSE (1==0)
#endif

// Input buffer:
//	max. unprocessed replys from the display

#define	MAX_GENIE_REPLYS	16

static struct genieReplyStruct genieReplys [MAX_GENIE_REPLYS] ;
static int genieReplysHead = 0 ;
static int genieReplysTail = 0 ;

#ifdef	GENIE_DEBUG
int genieAck = FALSE ;
int genieNak = FALSE ;
int genieChecksumErrors = 0 ;
int genieTimeouts       = 0 ;
#else
static int genieAck = FALSE ;
static int genieNak = FALSE ;
static int genieChecksumErrors = 0 ;
static int genieTimeouts       = 0 ;
#endif

static pthread_mutex_t genieMutex ;

static int genieFd = -1;


/*
 * genieOpen:
 *	Open and initialise the serial port, setting all the right
 *	port parameters - or as many as are required - hopefully!
 *********************************************************************************
 */

static int genieOpen (char *device, int baud)
{
  struct termios options ;
  speed_t myBaud ;
  int     status, fd ;

  switch (baud)
  {
    case     50:	myBaud =     B50 ; break ;
    case     75:	myBaud =     B75 ; break ;
    case    110:	myBaud =    B110 ; break ;
    case    134:	myBaud =    B134 ; break ;
    case    150:	myBaud =    B150 ; break ;
    case    200:	myBaud =    B200 ; break ;
    case    300:	myBaud =    B300 ; break ;
    case    600:	myBaud =    B600 ; break ;
    case   1200:	myBaud =   B1200 ; break ;
    case   1800:	myBaud =   B1800 ; break ;
    case   2400:	myBaud =   B2400 ; break ;
    case   9600:	myBaud =   B9600 ; break ;
    case  19200:	myBaud =  B19200 ; break ;
    case  38400:	myBaud =  B38400 ; break ;
    case  57600:	myBaud =  B57600 ; break ;
    case 115200:	myBaud = B115200 ; break ;
    case 230400:	myBaud = B230400 ; break ;

    default:
      return -2 ;
  }

  if ((fd = open (device, O_RDWR | O_NOCTTY | O_NDELAY | O_NONBLOCK)) == -1)
    return -1 ;

  fcntl (fd, F_SETFL, O_RDWR) ;

// Get and modify current options:

  tcgetattr (fd, &options) ;

    cfmakeraw   (&options) ;
    cfsetispeed (&options, myBaud) ;
    cfsetospeed (&options, myBaud) ;

    options.c_cflag |= (CLOCAL | CREAD) ;
    options.c_cflag &= ~PARENB ;
    options.c_cflag &= ~CSTOPB ;
    options.c_cflag &= ~CSIZE ;
    options.c_cflag |= CS8 ;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG) ;
    options.c_oflag &= ~OPOST ;

    options.c_cc [VMIN]  =   0 ;
    options.c_cc [VTIME] = 100 ;	// Ten seconds (100 deciseconds)

  tcsetattr (fd, TCSANOW | TCSAFLUSH, &options) ;

  ioctl (fd, TIOCMGET, &status);

  status |= TIOCM_DTR ;
  status |= TIOCM_RTS ;

  ioctl (fd, TIOCMSET, &status);

  usleep (10000) ;	// 10mS

  return fd ;
}


/*
 * genieFlush:
 *	Flush the serial buffers (both tx & rx)
 *********************************************************************************
 */

static void genieFlush (int fd)
{
  tcflush (fd, TCIOFLUSH) ;
}


/*
 * genieClose:
 *	Release the serial port and any other data we have.
 *********************************************************************************
 */

void genieClose (void)
{
  close (genieFd) ;
}


/*
 * genieDataAvail:
 *	Return the number of bytes of data avalable to be read in the serial port
 *********************************************************************************
 */

static int genieDataAvail (int fd)
{
  int result ;

  if (ioctl (fd, FIONREAD, &result) == -1)
    return -1 ;

  return result ;
}


/*
 * Support timing functions. These are based on those in wiringPi
 *********************************************************************************
 */

static unsigned long long epoch ;

static unsigned int millis (void)
{
  struct timeval tv ;
  unsigned long long t1 ;

  gettimeofday (&tv, NULL) ;

  t1 = (tv.tv_sec * 1000000 + tv.tv_usec) / 1000 ;

  return (unsigned int)(t1 - epoch) ;
}

static void delay (unsigned int howLong)
{
  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = (time_t)(howLong / 1000) ;
  sleeper.tv_nsec = (long)(howLong % 1000) * 1000000 ;

  nanosleep (&sleeper, &dummy) ;
}

static void delayMicroseconds (unsigned int howLong)
{
  struct timespec sleeper, dummy ;

  sleeper.tv_sec  = 0 ;
  sleeper.tv_nsec = (long)(howLong * 1000) ;

  nanosleep (&sleeper, &dummy) ;
}


/*
 * genieGetchar:
 *	Return a single character from the device, or -1 if nothing
 *	avalable in 5mS.
 *********************************************************************************
 */

static int genieGetchar (void)
{
  unsigned int timeUp = millis () + 5 ;
  unsigned char x ;

  while (millis () < timeUp)
    if (genieDataAvail (genieFd))
    {
      if (read (genieFd, &x, 1) == 1)
	return ((int)x) & 0xFF ;
      return -1 ;
    }
    else
      delayMicroseconds (101) ;
  
  return -1 ;
}


/*
 * geniePutchar:
 *	Send a single character (byte) to the Genie display
 *********************************************************************************
 */

static void geniePutchar (int data)
{
  unsigned char c = (unsigned char)data ;
  write (genieFd, &c, 1) ;
}


/*
 * genieReplyListener:
 *	Listen for bytes from the Genie display and build them into
 *	messages, and store them in the message queue
 *********************************************************************************
 */

static void *genieReplyListener (void *data)
{
  struct sched_param sched ;
  int pri = 20 ;

  unsigned int cmd, object, index, msb, lsb, csum ;
  struct genieReplyStruct *reply ;
  int next ;


// Set to a real-time priority

  memset (&sched, 0, sizeof(sched)) ;

  if (pri > sched_get_priority_max (SCHED_RR))
    pri = sched_get_priority_max (SCHED_RR) ;

  sched.sched_priority = pri ;
  sched_setscheduler (0, SCHED_RR, &sched) ;

// Make sure the serial port is actually open

  while (genieFd == -1)
    delay (1) ;

// Loop, forever, catching events coming back from the display

  for (;;)
  {
    while ((cmd = genieGetchar ()) == -1)
      ;

    if (cmd == GENIE_ACK)
      { genieAck = TRUE ; continue ; }
    if (cmd == GENIE_NAK)
      { genieNak = TRUE ; continue ; }

    csum  = cmd ;

    if ((object = genieGetchar ()) == -1) { ++genieTimeouts ; continue ; } ; csum ^= object ;
    if ((index  = genieGetchar ()) == -1) { ++genieTimeouts ; continue ; } ; csum ^= index ;
    if ((msb    = genieGetchar ()) == -1) { ++genieTimeouts ; continue ; } ; csum ^= msb ;
    if ((lsb    = genieGetchar ()) == -1) { ++genieTimeouts ; continue ; } ; csum ^= lsb ;

    if (genieGetchar () != csum)
    {
      ++genieChecksumErrors ;
      continue ;
    }

// We have valid data - store it into the buffer

    next = (genieReplysHead + 1) & (MAX_GENIE_REPLYS - 1) ;
    if (next != genieReplysTail)			// Discard rather than overflow
    {
      reply = &genieReplys [genieReplysHead] ;
      reply->cmd    = cmd ;
      reply->object = object ;
      reply->index  = index ;
      reply->data   = msb << 8 | lsb ;
      genieReplysHead = next ;
    }
  }

  return (void *)NULL ;
}


/*
 * genieReplyAvail:
 *	Return TRUE if there are pending messages from the display
 *********************************************************************************
 */

int genieReplyAvail (void)
{
  return (genieReplysHead != genieReplysTail) ;
}


/*
 * genieGetReply:
 *	Get the next message out of the Genie Reply queue, or
 *	wait until a message has been sent from the display
 *********************************************************************************
 */

void genieGetReply (struct genieReplyStruct *reply)
{
  while (!genieReplyAvail ())
    delay (1) ;

  memcpy (reply, &genieReplys [genieReplysTail], sizeof (struct genieReplyStruct)) ;

  genieReplysTail = (genieReplysTail + 1) & (MAX_GENIE_REPLYS - 1) ;
}


/*
 * genieReadObj:
 *	Send a read object command to the Genie display and get the result back
 *********************************************************************************
 */

static int _genieReadObj (int object, int index)
{
  struct genieReplyStruct reply ;
  unsigned int timeUp, checksum ;


// Discard any pending replys

  while (genieReplyAvail ())
    genieGetReply (&reply) ;

  genieAck = genieNak = FALSE ;

  geniePutchar (GENIE_READ_OBJ) ; checksum   = GENIE_READ_OBJ ;
  geniePutchar (object) ;         checksum  ^= object ;
  geniePutchar (index) ;          checksum  ^= index ;
  geniePutchar (checksum) ;

// Wait up to 50mS for a reply
//	Note: @9600 baud 5 characters will take 5mS!

  for (timeUp = millis () + 50 ; millis () < timeUp ;)
  {
    if (genieNak)
      return -1 ;

    if (genieReplyAvail ())
    {
      genieGetReply (&reply) ;
      if ((reply.cmd == GENIE_REPORT_OBJ) && (reply.object == object) && (reply.index == index))
	return reply.data ;
    }

    delayMicroseconds (101) ;
  }

  return -1 ;
}

int genieReadObj (int object, int index)
{
  int result ;
 
  pthread_mutex_lock   (&genieMutex) ;
    result = _genieReadObj (object, index) ;
  pthread_mutex_unlock (&genieMutex) ;

  return result ;

}


/*
 * genieWriteObj:
 *	Write data to an object on the display
 *********************************************************************************
 */

static int _genieWriteObj (int object, int index, unsigned int data)
{
  unsigned int checksum, msb, lsb ;

  lsb = (data >> 0) & 0xFF ;
  msb = (data >> 8) & 0xFF ;

  genieAck = genieNak = FALSE ;

  geniePutchar (GENIE_WRITE_OBJ) ; checksum  = GENIE_WRITE_OBJ ;
  geniePutchar (object) ;          checksum ^= object ;
  geniePutchar (index) ;           checksum ^= index ;
  geniePutchar (msb) ;             checksum ^= msb ;
  geniePutchar (lsb) ;             checksum ^= lsb ;
  geniePutchar (checksum) ;

// TODO: Really ought to timeout here, but if the display doesn't
//	respond, then it's probably game over anyway.

  while ((genieAck == FALSE) && (genieNak == FALSE))
    delay (1) ;

  return 0 ;
}

int genieWriteObj (int object, int index, unsigned int data)
{
  int result ;

  pthread_mutex_lock   (&genieMutex) ;
    result = _genieWriteObj (object, index, data) ;
  pthread_mutex_unlock (&genieMutex) ;

  return result ;
}


/*
 * genieWriteContrast:
 *	Alter the display contrast (backlight)
 *********************************************************************************
 */

static int _genieWriteContrast (int value)
{
  unsigned int checksum ;

  genieAck = genieNak = FALSE ;

  geniePutchar (GENIE_WRITE_CONTRAST) ; checksum  = GENIE_WRITE_CONTRAST ;
  geniePutchar (value) ;                checksum ^= value ;
  geniePutchar (checksum) ;

// TODO: Really ought to timeout here, but if the display doesn't
//	respond, then it's probably game over anyway.

  while ((genieAck == FALSE) && (genieNak == FALSE))
    delay (1) ;

  return 0 ;
}

int genieWriteContrast (int value)
{
  int result ;

  pthread_mutex_lock   (&genieMutex) ;
    result = _genieWriteContrast (value) ;
  pthread_mutex_unlock (&genieMutex) ;

  return result ;
}

/*
 * genieWriteStr:
 * genieWriteStrU:
 *	Write a string to the display (ASCII, or Unicode)
 *	There is only one string type object.
 *********************************************************************************
 */

static int _genieWriteStr (int index, char *string)
{
  char *p ;
  unsigned int checksum ;
  int len = strlen (string) ;

  if (len > 255)
    return -1 ;

  genieAck = genieNak = FALSE ;

  geniePutchar (GENIE_WRITE_STR) ;    checksum  = GENIE_WRITE_STR ;
  geniePutchar (index) ;              checksum ^= index ;
  geniePutchar ((unsigned char)len) ; checksum ^= len ;
  for (p = string ; *p ; ++p)
  {
    geniePutchar (*p) ;
    checksum ^= *p ;
  }
  geniePutchar (checksum) ;

// TODO: Really ought to timeout here, but if the display doesn't
//	respond, then it's probably game over anyway.

  while ((genieAck == FALSE) && (genieNak == FALSE))
    delay (1) ;

  return 0 ;
}

static int _genieWriteStrU (int index, char *string)
{
  char *p ;
  unsigned int checksum ;
  int len = strlen (string) ;

  if (len > 255)
    return -1 ;

  genieAck = genieNak = FALSE ;

  geniePutchar (GENIE_WRITE_STRU) ;   checksum  = GENIE_WRITE_STRU ;
  geniePutchar (index) ;              checksum ^= index ;
  geniePutchar ((unsigned char)len) ; checksum ^= len ;
  for (p = string ; *p ; ++p)
  {
    geniePutchar (*p) ;
    checksum ^= *p ;
  }
  geniePutchar (checksum) ;

// TODO: Really ought to timeout here, but if the display doesn't
//	respond, then it's probably game over anyway.

  while ((genieAck == FALSE) && (genieNak == FALSE))
    delay (1) ;

  return 0 ;
}


int genieWriteStr (int index, char *string)
{
  int result ;

  pthread_mutex_lock   (&genieMutex) ;
    result = _genieWriteStr (index, string) ;
  pthread_mutex_unlock (&genieMutex) ;

  return result ;
}

int genieWriteStrU (int index, char *string)
{
  int result ;

  pthread_mutex_lock   (&genieMutex) ;
    result = _genieWriteStrU (index, string) ;
  pthread_mutex_unlock (&genieMutex) ;

  return result ;
}



/*
 * genieSetup:
 *	Initialise the Genie Display system
 *********************************************************************************
 */

int genieSetup (char *device, int baud)
{
  int i ;

  pthread_t myThread ;
  struct timeval tv ;

  if ((genieFd = genieOpen (device, baud)) < 0)
    return -1 ;

  genieFlush (genieFd) ;

  gettimeofday (&tv, NULL) ;
  epoch = (tv.tv_sec * 1000000 + tv.tv_usec) / 1000 ;

// Try to overcome a bug with the Raspberry Pi (or indeed, any other serial
//	port that sends a garbage character when you first open it),
//	by sending out dummy characters until we get a NAK back, hopefully
//	then the display sequencer will be in a stable state.

  for (i = 0 ; i < 10 ; ++i)
  {
    geniePutchar ('X') ;
    if (genieGetchar () == GENIE_NAK)
      break ;
  }

  return pthread_create (&myThread, NULL, genieReplyListener, NULL) ;
}
