/******************************************************************************
 * $Id: nmea.cpp,v 1.9 2006/12/03 21:31:41 dsr Exp $
 *
 * Project:  OpenCPN
 * Purpose:  NMEA Data Object
 * Author:   David Register
 *
 ***************************************************************************
 *   Copyright (C) $YEAR$ by $AUTHOR$   *
 *   $EMAIL$   *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 *
 * $Log: nmea.cpp,v $
 * Revision 1.9  2006/12/03 21:31:41  dsr
 * Implement new ctor with window ID argument.
 * Change time set logic to run once per session only.
 *
 * Revision 1.8  2006/11/01 02:16:37  dsr
 * AIS Support Touchup
 *

 */


#include "wx/wxprec.h"

#ifndef  WX_PRECOMP
  #include "wx/wx.h"
#endif //precompiled headers

#include "wx/tokenzr.h"

#include <stdlib.h>
#include <math.h>
#include <time.h>

#include "dychart.h"

#include "nmea.h"
#include "chart1.h"


#ifdef __WXMSW__
    #ifdef dyUSE_MSW_SERCOMM
    #include "sercomm.h"
    #endif
#endif

CPL_CVSID("$Id: nmea.cpp,v 1.9 2006/12/03 21:31:41 dsr Exp $");

//    Forward Declarations

extern NMEAWindow *nmea;
extern float            gLat, gLon, gSog, gCog;
extern bool             bAutoPilotOut;



extern "C" void toDMS(double a, char *bufp, int bufplen);
extern "C" void toDMM(double a, char *bufp, int bufplen);

extern int    user_user_id;
extern int    file_user_id;

extern wxString          *phost_name;

extern OCP_NMEA_Thread   *pNMEA_Thread;

int s_dns_test_flag;


//------------------------------------------------------------------------------
//    NMEA Window Implementation
//------------------------------------------------------------------------------
BEGIN_EVENT_TABLE(NMEAWindow, wxWindow)
  EVT_PAINT(NMEAWindow::OnPaint)
  EVT_ACTIVATE(NMEAWindow::OnActivate)
  EVT_CLOSE(NMEAWindow::OnCloseWindow)

  EVT_SOCKET(SOCKET_ID, NMEAWindow::OnSocketEvent)
  EVT_TIMER(TIMER_NMEA1, NMEAWindow::OnTimerNMEA)

  END_EVENT_TABLE()

// constructor
NMEAWindow::NMEAWindow(int window_id, wxFrame *frame, const wxString& NMEADataSource):
      wxWindow(frame, window_id,     wxPoint(20,20), wxDefaultSize, wxSIMPLE_BORDER)

{

      parent_frame = (MyFrame *)frame;
      m_pParentEventHandler = parent_frame->GetEventHandler();

      pNMEA_Thread = NULL;
      m_sock = NULL;

      TimerNMEA.SetOwner(this, TIMER_NMEA1);
      TimerNMEA.Stop();

      m_pdata_source_string = new wxString(NMEADataSource);


//      Create and manage NMEA data stream source

//    Decide upon NMEA source
      wxLogMessage("NMEA Data Source is....%s",m_pdata_source_string->c_str());

//      NMEA Data Source is private TCP/IP Server
      if(m_pdata_source_string->Contains("GPSD"))
      {
            wxString NMEA_data_ip;
            NMEA_data_ip = m_pdata_source_string->Mid(5);         // extract the IP

// Create the socket
            m_sock = new wxSocketClient();

// Setup the event handler and subscribe to most events
            m_sock->SetEventHandler(*this, SOCKET_ID);

            m_sock->SetNotify(wxSOCKET_CONNECTION_FLAG |
                    wxSOCKET_INPUT_FLAG |
                    wxSOCKET_LOST_FLAG);
            m_sock->Notify(TRUE);

            m_busy = FALSE;


//    Build the target address

//    n.b. Win98
//    wxIPV4address::Hostname() uses sockets function gethostbyname() for address resolution
//    Implications...Either name target must exist in c:\windows\hosts, or
//                            a DNS server must be active on the network.
//    If neither true, then wxIPV4address::Hostname() will block (forever?)....
//
//    Workaround....
//    Use a thread to try the name lookup, in case it hangs

            DNSTestThread *ptest_thread = NULL;
            ptest_thread = new DNSTestThread(NMEA_data_ip);

            ptest_thread->Run();                      // Run the thread from ::Entry()


//    Sleep and loop for N seconds
#define SLEEP_TEST_SEC  2

            for(int is=0 ; is<SLEEP_TEST_SEC * 10 ; is++)
            {
                  wxMilliSleep(100);
                  if(s_dns_test_flag)
                        break;
            }

            if(!s_dns_test_flag)
            {

                  wxString msg(NMEA_data_ip);
                  msg.Prepend("Could not resolve TCP/IP host '");
                  msg.Append("'\n Suggestion: Try 'xxx.xxx.xxx.xxx' notation");
                  wxMessageDialog md(this, msg, "OpenCPN Message", wxICON_ERROR );
                  md.ShowModal();

                  m_sock->Notify(FALSE);
                  m_sock->Destroy();
                  m_sock = NULL;

                  return;
            }


            //      Resolved the name, somehow, so Connect() the socket
            addr.Hostname(NMEA_data_ip);
            addr.Service(GPSD_PORT_NUMBER);
            m_sock->Connect(addr, FALSE);       // Non-blocking connect

            TimerNMEA.Start(TIMER_NMEA_MSEC,wxTIMER_CONTINUOUS);
      }


//    NMEA Data Source is specified serial port

      else if(m_pdata_source_string->Contains("Serial"))
      {
          wxString comx;
          comx =  m_pdata_source_string->Mid(7);        // either "COM1" style or "/dev/ttyS0" style

#ifdef __WXMSW__

//  As a quick check, verify that the specified port is available
            HANDLE m_hSerialComm = CreateFile(comx.c_str(),       // Port Name
                                             GENERIC_READ,
                                             0,
                                             NULL,
                                             OPEN_EXISTING,
                                             0,
                                             NULL);

            if(m_hSerialComm == INVALID_HANDLE_VALUE)
            {
                  wxString msg(comx);
                  msg.Prepend("  Could not open serial port '");
                  msg.Append("'\nSuggestion: Try closing other applications.");
                  wxMessageDialog md(this, msg, "OpenCPN Message", wxICON_ERROR );
                  md.ShowModal();

                  return;
            }

            else
                  CloseHandle(m_hSerialComm);

//    Kick off the NMEA RX thread
            pNMEA_Thread = new OCP_NMEA_Thread(frame, comx);
            pNMEA_Thread->Run();
#endif


#ifdef __LINUX__
//    Kick off the NMEA RX thread
            pNMEA_Thread = new OCP_NMEA_Thread(frame, comx);
            pNMEA_Thread->Run();
#endif

      }

      Hide();
}


NMEAWindow::~NMEAWindow()
{
      delete m_pdata_source_string;
}

void NMEAWindow::OnCloseWindow(wxCloseEvent& event)
{

//    Kill off the NMEA Socket if alive
      if(m_sock)
      {
            m_sock->Notify(FALSE);
            m_sock->Destroy();
            TimerNMEA.Stop();
      }


//    Kill off the NMEA RX Thread if alive
      if(pNMEA_Thread)
      {
          pNMEA_Thread->Delete();         //was kill();
#ifdef __WXMSW__
          wxSleep(2);
#else
          sleep(2);
#endif
      }

}


void NMEAWindow::GetSource(wxString& source)
{
      source = *m_pdata_source_string;
}



void NMEAWindow::OnActivate(wxActivateEvent& event)
{
}

void NMEAWindow::OnPaint(wxPaintEvent& event)
{
      wxPaintDC dc(this);
}

void NMEAWindow::Pause(void)
{
      TimerNMEA.Stop();

      if(m_sock)
            m_sock->Notify(FALSE);
}

void NMEAWindow::UnPause(void)
{
    TimerNMEA.Start(TIMER_NMEA_MSEC,wxTIMER_CONTINUOUS);

      if(m_sock)
            m_sock->Notify(TRUE);
}



void NMEAWindow::OnSocketEvent(wxSocketEvent& event)
{

#define RD_BUF_SIZE    200

    int nBytes;
    unsigned char *bp;
    unsigned char buf[RD_BUF_SIZE + 1];
    int char_count;
    wxString token;
    double dglat, dglon, dgcog, dgsog;
    double dtime;
    wxDateTime fix_time;

  switch(event.GetSocketEvent())
  {
      case wxSOCKET_INPUT :                     // from gpsd Daemon
            m_sock->SetFlags(wxSOCKET_NOWAIT);


//          Read the reply, one character at a time, looking for 0x0a (lf)
            bp = buf;
            char_count = 0;

            while (char_count < RD_BUF_SIZE)
            {
                m_sock->Read(bp, 1);
                nBytes = m_sock->LastCount();
                if(*bp == 0x0a)
                    break;

                bp++;
                char_count++;
            }

            *bp = 0;                        // end string

//          Validate the string

// a debug test
//            if(1)
//                printf("%s", buf);

            if(!strncmp((const char *)buf, "GPSD", 4))
            {

                if(buf[7] != '?')           // valid data?
                {
                    wxStringTokenizer tkz(buf, " ");
                    token = tkz.GetNextToken();

                    token = tkz.GetNextToken();
                    if(token.ToDouble(&dtime))
                    {
                        fix_time.Set((time_t) floor(dtime));
                        wxString fix_time_format = fix_time.Format("%Y-%m-%dT%H:%M:%S");  // this should show as LOCAL
                    }


                    token = tkz.GetNextToken();         // skip to lat

                    token = tkz.GetNextToken();
                    if(token.ToDouble(&dglat))
                        gLat = dglat;

                    token = tkz.GetNextToken();
                    if(token.ToDouble(&dglon))
                        gLon = dglon;

                    token = tkz.GetNextToken();         // skip to tmg
                    token = tkz.GetNextToken();
                    token = tkz.GetNextToken();

                    token = tkz.GetNextToken();
                    if(token.ToDouble(&dgcog))
                        gCog = dgcog;

                    token = tkz.GetNextToken();
                    if(token.ToDouble(&dgsog))
                        gSog = dgsog;

//      Use the fix time to update the local clock
                    if(fix_time.IsValid())
                    {

//          Compare the server (fix) time to the current system time
                        wxDateTime sdt;
                        sdt.SetToCurrent();
                        wxDateTime cwxft = fix_time;                  // take a copy
                        wxTimeSpan ts;
                        ts = cwxft.Subtract(sdt);

                        int b = (ts.GetSeconds()).ToLong();
//          Correct system time if necessary
//      Only set the time once per session, and only if wrong by more than 1 minute.

                        if((abs(b) > 60) && (parent_frame->m_bTimeIsSet == false))
                        {

#ifdef __WXMSW__
//      Fix up the fix_time to convert to GMT
                              fix_time = fix_time.ToGMT();

//    Code snippet following borrowed from wxDateCtrl, MSW

                              const wxDateTime::Tm tm(fix_time.GetTm());


                              SYSTEMTIME stm;
                              stm.wYear = (WXWORD)tm.year;
                              stm.wMonth = (WXWORD)(tm.mon - wxDateTime::Jan + 1);
                              stm.wDay = tm.mday;

                              stm.wDayOfWeek = 0;
                              stm.wHour = fix_time.GetHour();
                              stm.wMinute = tm.min;
                              stm.wSecond = tm.sec;
                              stm.wMilliseconds = 0;

                              ::SetSystemTime(&stm);            // in GMT


#else


//      This contortion sets the system date/time on host
//      Requires the following line in /etc/sudoers
//          nav ALL=NOPASSWD:/bin/date -s ????????

                            wxLogMessage("Time set, delta t is %d", b);

                            wxString sdate(fix_time.Format("%D"));
                            sdate.Prepend("sudo /bin/date -s ");
                        //    printf("%s\n", sdate.c_str());
                            wxExecute(sdate, wxEXEC_ASYNC);

                            wxString g(fix_time.Format("%T"));
                            g.Prepend("sudo /bin/date -s ");
                        //    printf("%s\n", g.c_str());
                            wxExecute(g, wxEXEC_ASYNC);

#endif      //__WXMSW__
                            if(parent_frame)
                            parent_frame->m_bTimeIsSet = true;

                        }           // if needs correction
                    }               // if valid time


//    Signal the main program thread

                    wxCommandEvent event( EVT_NMEA,  ID_NMEA_WINDOW );
                    event.SetEventObject( (wxObject *)this );
                    event.SetExtraLong(EVT_NMEA_DIRECT);
                    m_pParentEventHandler->AddPendingEvent(event);
                }
            }



    case wxSOCKET_LOST       :
    case wxSOCKET_CONNECTION :
    default                  :
          break;
  }

}



void NMEAWindow::OnTimerNMEA(wxTimerEvent& event)
{
      TimerNMEA.Stop();

      if(m_sock)
      {
        if(m_sock->IsConnected())
        {
            unsigned char c = 'O';
            m_sock->Write(&c, 1);
        }
        else                                    // try to connect
        {
            m_sock->Connect(addr, FALSE);       // Non-blocking connect
        }
      }



//--------------TEST
#if(0)
      if(1)
      {
            gCog = 290.;
            gSog = 7.;

            float PI = 3.14159;
            float pred_lat = gLat +  (cos(gCog * PI / 180) * gSog * (1. / 60.) / 3600.)/(cos(gLat * PI/180.));
            float pred_lon = gLon +  (sin(gCog * PI / 180) * gSog * (1. / 60.) / 3600.)/(cos(gLat * PI/180.));

            gLat = pred_lat;
            gLon = pred_lon;
      }


#endif

      char buf[80];
      strcpy(buf, "                        ");        // ugly
      toDMM(gLat, buf, 20);
      int i = strlen(buf);
      buf[i++] = ' ';
      buf[i++] = ' ';

      toDMM(gLon, &buf[i], 20);

      if(parent_frame->pStatusBar)
            parent_frame->SetStatusText(buf, 3);


      TimerNMEA.Start(TIMER_NMEA_MSEC,wxTIMER_CONTINUOUS);
}



//-------------------------------------------------------------------------------------------------------------
//
//    A simple thread to test host name resolution without blocking the main thread
//    Dontcha' love socket's name resolution logic??
//
//-------------------------------------------------------------------------------------------------------------
DNSTestThread::DNSTestThread(wxString &name_or_ip)
{
      m_pip = new wxString(name_or_ip);

      Create();
}

DNSTestThread::~DNSTestThread()
{
      delete m_pip;
}


void *DNSTestThread::Entry()
{
      s_dns_test_flag = 0;

      wxIPV4address     addr;
      addr.Hostname(*m_pip);                          // this may block forever if DNS is not active

      s_dns_test_flag = 1;                            // came back OK
      return NULL;
}


//-------------------------------------------------------------------------------------------------------------
//
//    NMEA Serial Input Thread
//
//    This thread manages reading the NMEA data stream from the declared NMEA serial port
//
//-------------------------------------------------------------------------------------------------------------

//          Inter-thread communication event implementation
DEFINE_EVENT_TYPE(EVT_NMEA)



//-------------------------------------------------------------------------------------------------------------
//    OCP_NMEA_Thread Static data store
//-------------------------------------------------------------------------------------------------------------

extern char                         rx_share_buffer[MAX_RX_MESSSAGE_SIZE];
extern unsigned int                 rx_share_buffer_length;
extern ENUM_BUFFER_STATE            rx_share_buffer_state;
extern wxMutex                      *ps_mutexProtectingTheRXBuffer;

//-------------------------------------------------------------------------------------------------------------
//    OCP_NMEA_Thread Implementation
//-------------------------------------------------------------------------------------------------------------

//    ctor

OCP_NMEA_Thread::OCP_NMEA_Thread(wxWindow *MainWindow, const char *pszPortName)
{

      m_pMainEventHandler = MainWindow->GetEventHandler();

      rx_share_buffer_state = RX_BUFFER_EMPTY;

      m_pPortName = new wxString(pszPortName);

      rx_buffer = new char[RX_BUFFER_SIZE + 1];
      put_ptr = rx_buffer;
      tak_ptr = rx_buffer;

      ps_mutexProtectingTheRXBuffer = new wxMutex;

      Create();
}

OCP_NMEA_Thread::~OCP_NMEA_Thread(void)
{
      delete rx_buffer;
}

void OCP_NMEA_Thread::OnExit(void)
{
    //  Mark the global status as dead, gone
    pNMEA_Thread = NULL;
}

#ifdef __LINUX__

#if defined (HAVE_SYS_TERMIOS_H)
#include <sys/termios.h>
#else
#if defined (HAVE_TERMIOS_H)
#include <termios.h>
#endif
#endif

#include <sys/termios.h>
#endif


//      Sadly, the thread itself must implement the underlying OS serial port
//      in a very machine specific way....

#ifdef __LINUX__
#if 1
//    Entry Point
void *OCP_NMEA_Thread::Entry()
{


    // Allocate the termios data structures

    pttyset = (termios *)malloc(sizeof (termios));
    pttyset_old = (termios *)malloc(sizeof (termios));

    // Open the serial port.
    //if ((m_gps_fd = open(m_pPortName->c_str(), O_RDWR|O_NONBLOCK|O_NOCTTY)) < 0)
    if ((m_gps_fd = open(m_pPortName->c_str(), O_RDWR|O_NOCTTY)) < 0)
    {
        wxLogMessage("NMEA input device open failed: %s\n", m_pPortName->c_str());
        return 0;
    }


    {
        (void)cfsetispeed(pttyset, B4800);
        (void)cfsetospeed(pttyset, (speed_t)B4800);
        (void)tcsetattr(m_gps_fd, TCSANOW, pttyset);
        (void)tcflush(m_gps_fd, TCIOFLUSH);
    }

    if (isatty(m_gps_fd)!=0)
    {

      /* Save original terminal parameters */
      if (tcgetattr(m_gps_fd,pttyset_old) != 0)
      {
          wxLogMessage("NMEA input device getattr failed: %s\n", m_pPortName->c_str());
          return 0;
      }
      (void)memcpy(pttyset, pttyset_old, sizeof(termios));

      //  Build the new parms off the old

      // Set blocking/timeout behaviour
      memset(pttyset->c_cc,0,sizeof(pttyset->c_cc));
//      pttyset->c_cc[VMIN] = 1;
      pttyset->c_cc[VTIME] = 11;                        // 1.1 sec timeout

      /*
      * No Flow Control
      */
      pttyset->c_cflag &= ~(PARENB | PARODD | CRTSCTS);
      pttyset->c_cflag |= CREAD | CLOCAL;
      pttyset->c_iflag = pttyset->c_oflag = pttyset->c_lflag = (tcflag_t) 0;

      int stopbits = 1;
      char parity = 'N';
      pttyset->c_iflag &=~ (PARMRK | INPCK);
      pttyset->c_cflag &=~ (CSIZE | CSTOPB | PARENB | PARODD);
      pttyset->c_cflag |= (stopbits==2 ? CS7|CSTOPB : CS8);
      switch (parity)
      {
          case 'E':
              pttyset->c_iflag |= INPCK;
              pttyset->c_cflag |= PARENB;
              break;
          case 'O':
              pttyset->c_iflag |= INPCK;
              pttyset->c_cflag |= PARENB | PARODD;
              break;
      }
      pttyset->c_cflag &=~ CSIZE;
      pttyset->c_cflag |= (CSIZE & (stopbits==2 ? CS7 : CS8));
      if (tcsetattr(m_gps_fd, TCSANOW, pttyset) != 0)
      {
          wxLogMessage("NMEA input device setattr failed: %s\n", m_pPortName->c_str());
          return 0;
      }


      (void)tcflush(m_gps_fd, TCIOFLUSH);
    }



    bool not_done = true;
    bool nl_found;

//    The main loop
    printf("starting\n");

    while(not_done)
    {
        if(TestDestroy())
        {
            not_done = false;                               // smooth exit
            printf("smooth exit\n");
        }
//    Blocking, timeout protected read of one character at a time
//    Timeout value is set by c_cc[VTIME]
//    Storing incoming characters in circular buffer
//    And watching for new line character
//     On new line character, send notification to parent


        char next_byte = 0;
        ssize_t newdata;
        newdata = read(m_gps_fd, &next_byte, 1);            // blocking read of one char
                                                            // return (-1) if no data available, timeout
        if(newdata > 0)
        {
            nl_found = false;

            *put_ptr++ = next_byte;
            if((put_ptr - rx_buffer) > RX_BUFFER_SIZE)
                put_ptr = rx_buffer;

            if(0x0a == next_byte)
                nl_found = true;


//    Found a NL char, thus end of message?
            if(nl_found)
            {
                char *tptr;
                char *ptmpbuf;
                char temp_buf[RX_BUFFER_SIZE];


//    If the shared buffer is available....
                if(ps_mutexProtectingTheRXBuffer->Lock() == wxMUTEX_NO_ERROR )
                {
                    if(RX_BUFFER_EMPTY == rx_share_buffer_state)
                    {

//    Copy the message into the rx_shared_buffer

                        tptr = tak_ptr;
                        ptmpbuf = temp_buf;

                        while((*tptr != 0x0a) && (tptr != put_ptr))
                        {
                            *ptmpbuf++ = *tptr++;

                            if((tptr - rx_buffer) > RX_BUFFER_SIZE)
                                tptr = rx_buffer;
                        }

                        if(*tptr == 0x0a)                                     // well formed sentence
                        {
                            *ptmpbuf++ = *tptr++;
                            if((tptr - rx_buffer) > RX_BUFFER_SIZE)
                                tptr = rx_buffer;

                            *ptmpbuf = 0;

                            tak_ptr = tptr;
                            strcpy(rx_share_buffer, temp_buf);

                            rx_share_buffer_state = RX_BUFFER_FULL;
                            rx_share_buffer_length = strlen(rx_share_buffer);

//    Signal the main program thread

                            wxCommandEvent event( EVT_NMEA, ID_NMEA_WINDOW );
                            event.SetEventObject( (wxObject *)this );
                            event.SetExtraLong(EVT_NMEA_PARSE_RX);
                            m_pMainEventHandler->AddPendingEvent(event);
                        }
                    }
                }

//    Release the MUTEX
                ps_mutexProtectingTheRXBuffer->Unlock();

            }                   //if nl
        }                       // if newdata > 0
    }                          // the big while...


    //          Close the port cleanly


        /* this is the clean way to do it */
//    pttyset_old->c_cflag |= HUPCL;
//    (void)tcsetattr(m_gps_fd,TCSANOW,pttyset_old);
    (void)close(m_gps_fd);

    free (pttyset);
    free (pttyset_old);


    return 0;

}
#endif



#endif          //__LINUX__


#ifdef __WXMSW__
//    Entry Point
void *OCP_NMEA_Thread::Entry()
{

      bool not_done;

//    Set up the serial port

      m_hSerialComm = CreateFile(m_pPortName->c_str(),      // Port Name
                                             GENERIC_READ,              // Desired Access
                                             0,                               // Shared Mode
                                             NULL,                            // Security
                                             OPEN_EXISTING,             // Creation Disposition
                                             0,
                                             NULL);                           // Non Overlapped

      if(m_hSerialComm == INVALID_HANDLE_VALUE)
      {
            error = ::GetLastError();
            goto fail_point;
      }


      if(!SetupComm(m_hSerialComm, 1024, 1024))
            goto fail_point;

      DCB dcbConfig;

      if(GetCommState(m_hSerialComm, &dcbConfig))           // Configuring Serial Port Settings
      {
            dcbConfig.BaudRate = 4800;
            dcbConfig.ByteSize = 8;
            dcbConfig.Parity = NOPARITY;
            dcbConfig.StopBits = ONESTOPBIT;
            dcbConfig.fBinary = TRUE;
            dcbConfig.fParity = TRUE;
      }

      else
            goto fail_point;

      if(!SetCommState(m_hSerialComm, &dcbConfig))
            goto fail_point;

      COMMTIMEOUTS commTimeout;

      if(GetCommTimeouts(m_hSerialComm, &commTimeout)) // Configuring Read & Write Time Outs
      {
            commTimeout.ReadIntervalTimeout = 1000*TimeOutInSec;
            commTimeout.ReadTotalTimeoutConstant = 1000*TimeOutInSec;
            commTimeout.ReadTotalTimeoutMultiplier = 0;
            commTimeout.WriteTotalTimeoutConstant = 1000*TimeOutInSec;
            commTimeout.WriteTotalTimeoutMultiplier = 0;
      }

      else
            goto fail_point;

      if(!SetCommTimeouts(m_hSerialComm, &commTimeout))
            goto fail_point;


//    Set up event specification
      DWORD dwEventMask;

      if(!SetCommMask(m_hSerialComm, EV_RXCHAR)) // Setting Event Type
            goto fail_point;


      not_done = true;
      bool nl_found;

//    The main loop

      while(not_done)
      {
            if(TestDestroy())
                  not_done = false;                               // smooth exit

//    Blocking read of one character at a time
//    Storing incoming characters in circular buffer
//    And watching for new line character
            if(WaitCommEvent(m_hSerialComm, &dwEventMask, NULL)) // Waiting For Event to Occur
            {
                  char szBuf;
                  DWORD dwIncommingReadSize;

                  do
                  {
                        nl_found = false;
                        dwIncommingReadSize = 0;


                        COMSTAT    ComStat ;
                        DWORD      dwErrorFlags;


                        ClearCommError( m_hSerialComm, &dwErrorFlags, &ComStat ) ;

                        if(ComStat.cbInQue )
                        {

                              if(ReadFile(m_hSerialComm, &szBuf, 1, &dwIncommingReadSize, NULL) != 0)
                              {
                                    if(dwIncommingReadSize > 0)
                                    {
                                          int nchar = dwIncommingReadSize;
                                          while(nchar)
                                          {
                                                *put_ptr++ = szBuf;
                                                if((put_ptr - rx_buffer) > RX_BUFFER_SIZE)
                                                      put_ptr = rx_buffer;

                                                if(0x0a == szBuf)
                                                      nl_found = true;

                                                nchar--;
                                          }
                                    }
                              }

                              else
                              {
                                    error = ::GetLastError();
                                    break;
                              }
                        }

//    Found a NL char, thus end of message?
                        if(nl_found)
                        {
                              char *tptr;
                              char *ptmpbuf;
                              char temp_buf[RX_BUFFER_SIZE];

                              bool partial = false;
                              while (!partial)
                              {

//    If the shared buffer is available....
                                    if(ps_mutexProtectingTheRXBuffer->Lock() == wxMUTEX_NO_ERROR )
                                    {
                                          if(RX_BUFFER_EMPTY == rx_share_buffer_state)
                                          {

//    Copy the message into the rx_shared_buffer

                                                tptr = tak_ptr;
                                                ptmpbuf = temp_buf;

                                                while((*tptr != 0x0a) && (tptr != put_ptr))
                                                {
                                                      *ptmpbuf++ = *tptr++;

                                                      if((tptr - rx_buffer) > RX_BUFFER_SIZE)
                                                            tptr = rx_buffer;
                                                }

                                                if(*tptr == 0x0a)                                     // well formed sentence
                                                {
                                                      *ptmpbuf++ = *tptr++;
                                                      if((tptr - rx_buffer) > RX_BUFFER_SIZE)
                                                            tptr = rx_buffer;

                                                      *ptmpbuf = 0;

                                                      tak_ptr = tptr;
                                                      strcpy(rx_share_buffer, temp_buf);

                                                      rx_share_buffer_state = RX_BUFFER_FULL;
                                                      rx_share_buffer_length = strlen(rx_share_buffer);

//    Signal the main program thread

                                                      wxCommandEvent event( EVT_NMEA, ID_NMEA_WINDOW);
                                                      event.SetEventObject( (wxObject *)this );
                                                      event.SetExtraLong(EVT_NMEA_PARSE_RX);
                                                      m_pMainEventHandler->AddPendingEvent(event);
                                                }
                                                else
                                                {
                                                      partial = true;
//                                                    wxLogMessage("partial");
                                                }
                                          }
                                    }

//    Release the MUTEX
                                    ps_mutexProtectingTheRXBuffer->Unlock();

                              }                 // while

                        }

                  } while(dwIncommingReadSize > 0);

//
            }


      }           // the big while...


fail_point:

      return 0;

}

#endif


//-------------------------------------------------------------------------------------------------------------
//
//    Autopilot Class Implementation
//
//-------------------------------------------------------------------------------------------------------------
BEGIN_EVENT_TABLE(AutoPilotWindow, wxWindow)
        EVT_CLOSE(AutoPilotWindow::OnCloseWindow)
END_EVENT_TABLE()

// Implement a constructor
AutoPilotWindow::AutoPilotWindow(wxFrame *frame, const wxString& AP_Port):
        wxWindow(frame, wxID_ANY,     wxPoint(20,20), wxSize(5,5), wxSIMPLE_BORDER)

{

      m_pdata_ap_port_string = new wxString(AP_Port);
      bOK = false;

//    Create and init the Serial Port for Autopilot control

      wxLogMessage("NMEA AutoPilot Port is....%s",m_pdata_ap_port_string->c_str());

      if(!m_pdata_ap_port_string->IsSameAs("None", false))
      {

#ifdef __WXMSW__
#ifdef dyUSE_MSW_SERCOMM
            pWinComm = NULL;
            pWinComm = new CSyncSerialComm(m_pdata_ap_port_string->c_str());        //COM2
            pWinComm->Open();
            pWinComm->ConfigPort(4800, 5);
            bAutoPilotOut = true;
#endif
#endif

#ifdef __LINUX__

            bOK = OpenPort();

            if(bOK)
                 bAutoPilotOut = true;
#endif
    }
    Hide();
}

AutoPilotWindow::~AutoPilotWindow()
{
    delete m_pdata_ap_port_string;
}

void AutoPilotWindow::GetAP_Port(wxString& source)
{
    source = *m_pdata_ap_port_string;
}

void AutoPilotWindow::OnCloseWindow(wxCloseEvent& event)
{
#ifdef __WXMSW__
#if dyUSE_MSW_SERCOMM
      delete pWinComm;
#endif
#endif

#ifdef __LINUX__
    bAutoPilotOut = false;
    if(bOK)
    {
        (void)close(m_ap_fd);
        free (pttyset);
        free (pttyset_old);
    }
#endif
}

bool AutoPilotWindow::OpenPort(void)
{
#ifdef __LINUX__
    // Allocate the termios data structures

    pttyset = (termios *)malloc(sizeof (termios));
    pttyset_old = (termios *)malloc(sizeof (termios));

            // Open the serial port.
    if ((m_ap_fd = open(m_pdata_ap_port_string->c_str(), O_RDWR|O_NONBLOCK|O_NOCTTY)) < 0)
    {
        wxLogMessage("Autopilot output device open failed: %s\n", m_pdata_ap_port_string->c_str());
        return false;
    }


    (void)cfsetispeed(pttyset, B4800);
    (void)cfsetospeed(pttyset, (speed_t)B4800);
    (void)tcsetattr(m_ap_fd, TCSANOW, pttyset);
    (void)tcflush(m_ap_fd, TCIOFLUSH);


    if (isatty(m_ap_fd)!=0)
    {

        /* Save original terminal parameters */
        if (tcgetattr(m_ap_fd,pttyset_old) != 0)
        {
            wxLogMessage("Autopilot output device getattr failed: %s\n", m_pdata_ap_port_string->c_str());
            return false;
        }
        (void)memcpy(pttyset, pttyset_old, sizeof(termios));

      //  Build the new parms off the old

      // Set blocking/timeout behaviour
//                memset(pttyset->c_cc,0,sizeof(pttyset->c_cc));

      /*
        * No Flow Control
      */
        pttyset->c_cflag &= ~(PARENB | PARODD | CRTSCTS);
        pttyset->c_cflag |= CREAD | CLOCAL;
        pttyset->c_iflag = pttyset->c_oflag = pttyset->c_lflag = (tcflag_t) 0;

        int stopbits = 1;
        char parity = 'N';
        pttyset->c_iflag &=~ (PARMRK | INPCK);
        pttyset->c_cflag &=~ (CSIZE | CSTOPB | PARENB | PARODD);
        pttyset->c_cflag |= (stopbits==2 ? CS7|CSTOPB : CS8);
        switch (parity)
        {
            case 'E':
                pttyset->c_iflag |= INPCK;
                pttyset->c_cflag |= PARENB;
                break;
            case 'O':
                pttyset->c_iflag |= INPCK;
                pttyset->c_cflag |= PARENB | PARODD;
                break;
        }
        pttyset->c_cflag &=~ CSIZE;
        pttyset->c_cflag |= (CSIZE & (stopbits==2 ? CS7 : CS8));
        if (tcsetattr(m_ap_fd, TCSANOW, pttyset) != 0)
        {
            wxLogMessage("Autopilot output device setattr failed: %s\n", m_pdata_ap_port_string->c_str());
            return false;
        }


        (void)tcflush(m_ap_fd, TCIOFLUSH);

        return true;
    }
#endif      //__LINUX__

    return false;

}




void AutoPilotWindow::AutopilotOut(const char *Sentence)
{
    int char_count = strlen(Sentence);

#ifdef __WXMSW__
#ifdef dyUSE_MSW_SERCOMM
      pWinComm->Write(Sentence, char_count);
#endif
#endif

#ifdef __LINUX__

     ssize_t status;
     status = write(m_ap_fd, Sentence, char_count);
//     ok = (status == (ssize_t)len);
//     (void)tcdrain(m_ap_fd);

#endif

}


