// W32LowLevelMonitor.cc --- ActivityMonitor for W32
//
// Copyright (C) 2007 Ray Satiro <raysatiro@yahoo.com>
// All rights reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//

/*
Alternate low-level activity monitor.

--
From MSDN, re WH_KEYBOARD_LL & WH_MOUSE_LL:

This hook is called in the context of the thread that 
installed it. The call is made by sending a message to 
the thread that installed the hook. 

Therefore, the thread that installed the hook must 
have a message loop. 
--

Because a low-level hook is called in the context of 
the thread that installed it, we can install the hook 
from a time critical thread, and run a message loop.

This should solve the Philips slowdown problem:
1. high priority callback thread returns immediately after 
posting a message to the dispatch thread
2. the dispatch thread processes and notifies main thread


LowLevelKeyboardProc Function ():
http://msdn2.microsoft.com/en-us/library/ms644985.aspx

LowLevelMouseProc Function ():
http://msdn2.microsoft.com/en-us/library/ms644986.aspx

Scheduling Priorities (Windows):
http://msdn2.microsoft.com/en-us/library/ms685100.aspx


jay satiro, workrave project, september 2007
*/

#include <assert.h>
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <windows.h>

#include "debug.hh"
#include "W32LowLevelMonitor.hh"
#include "Harpoon.hh"


W32LowLevelMonitor *W32LowLevelMonitor::singleton = NULL;

W32LowLevelMonitor::thread_struct *W32LowLevelMonitor::dispatch = NULL;
W32LowLevelMonitor::thread_struct *W32LowLevelMonitor::callback = NULL;

volatile HHOOK W32LowLevelMonitor::k_hook = NULL;
volatile HHOOK W32LowLevelMonitor::m_hook = NULL;

HMODULE W32LowLevelMonitor::process_handle = NULL;
/* FYI:
MS type BOOL is type int.
Therefore BOOL functions can return -1.
MSDN notes GetMessage BOOL return is not only 0,1 but also -1.
*/
BOOL ( WINAPI *W32LowLevelMonitor::GetMessageW ) 
    ( LPMSG, HWND, UINT, UINT ) = NULL;
BOOL ( WINAPI *W32LowLevelMonitor::PeekMessageW )
    ( LPMSG, HWND, UINT, UINT, UINT ) = NULL;
BOOL ( WINAPI *W32LowLevelMonitor::PostThreadMessageW )
    ( DWORD, UINT, WPARAM, LPARAM ) = NULL;
HHOOK ( WINAPI *W32LowLevelMonitor::SetWindowsHookExW )
    ( int, HOOKPROC, HINSTANCE, DWORD ) = NULL;
BOOL ( WINAPI *W32LowLevelMonitor::SwitchToThread ) ( void ) = NULL;


W32LowLevelMonitor::W32LowLevelMonitor()
{
  TRACE_ENTER( "W32LowLevelMonitor::W32LowLevelMonitor" );
  
  if( singleton != NULL )
    {
      TRACE_RETURN( " singleton != NULL " );
      return;
    }
  
  singleton = this;
  
  dispatch = new thread_struct;
  dispatch->name = "Dispatch";
  
  callback = new thread_struct;
  callback->name = "Callback";
  
  k_hook = NULL;
  m_hook = NULL;
  
  TRACE_EXIT();
}


W32LowLevelMonitor::~W32LowLevelMonitor()
{
  TRACE_ENTER( "W32LowLevelMonitor::~W32LowLevelMonitor" );
  
  if( singleton != this )
    {
      TRACE_RETURN( " singleton != this " );
      return;
    }
  
  terminate();
  
  delete dispatch;
  delete callback;
  
  dispatch = NULL;
  callback = NULL;
  singleton = NULL;
  
  TRACE_EXIT();
}


bool W32LowLevelMonitor::check_api()
{
  TRACE_ENTER( "W32LowLevelMonitor::check_api" );
  
  process_handle = GetModuleHandle( NULL );
  HMODULE user32_handle = GetModuleHandleA( "user32.dll" );
  
  GetMessageW = ( BOOL ( WINAPI * ) ( LPMSG, HWND, UINT, UINT ) )
      GetProcAddress( user32_handle, "GetMessageW" );
  
  PeekMessageW = ( BOOL ( WINAPI * ) ( LPMSG, HWND, UINT, UINT, UINT ) )
      GetProcAddress( user32_handle, "PeekMessageW" );
  
  PostThreadMessageW = ( BOOL ( WINAPI * ) ( DWORD, UINT, WPARAM, LPARAM ) )
      GetProcAddress( user32_handle, "PostThreadMessageW" );
  
  SetWindowsHookExW = ( HHOOK ( WINAPI * ) ( int, HOOKPROC, HINSTANCE, DWORD ) )
      GetProcAddress( user32_handle, "SetWindowsHookExW" );
  
  SwitchToThread = ( BOOL ( WINAPI * ) ( void ) )
      GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "SwitchToThread" );
  
  if( process_handle && GetMessageW && PeekMessageW && 
      PostThreadMessageW && SetWindowsHookExW && SwitchToThread )
    {
      TRACE_EXIT();
      return true;
    }
  else
    {
      TRACE_MSG( " failed." );
      TRACE_MSG( "process_handle == " << process_handle );
      TRACE_MSG( "GetMessageW == " << GetMessageW );
      TRACE_MSG( "PeekMessageW == " << PeekMessageW );
      TRACE_MSG( "PostThreadMessageW == " << PostThreadMessageW );
      TRACE_MSG( "SetWindowsHookExW == " << SetWindowsHookExW );
      TRACE_MSG( "SwitchToThread == " << SwitchToThread );
      TRACE_EXIT();
      return false;
    }
}


bool W32LowLevelMonitor::init()
{
  TRACE_ENTER( "W32LowLevelMonitor::init" );
  
  if( singleton != this )
    {
      TRACE_RETURN( " singleton != this " );
      return false;
    }
  
  if( !check_api() )
    {
      TRACE_RETURN( " : init failed. " );
      return false;
    }

  terminate();
  
  dispatch->handle = 
      CreateThread( NULL, 0, thread_Dispatch, this, 0, &dispatch->id );
  
  if( !wait_for_thread_queue( dispatch ) )
    {
      terminate();
      TRACE_EXIT();
      return false;
    }
  
  callback->handle = 
      CreateThread( NULL, 0, thread_Callback, this, 0, &callback->id );
  
  if( !wait_for_thread_queue( callback ) )
    {
      terminate();
      TRACE_EXIT();
      return false;
    }
  
  Harpoon::init(NULL);
  
  TRACE_EXIT();
  return true;
}

bool W32LowLevelMonitor::wait_for_thread_queue( thread_struct *thread )
{
  TRACE_ENTER_MSG( "W32LowLevelMonitor::wait_for_thread_queue : ", thread->name);
  
  if( !thread->handle || !thread->id )
    {
      TRACE_RETURN( " thread: creation failed." );
      return false;
    }
  
  DWORD thread_exit_code;
  
  do
    {
      thread_exit_code = 0;
      GetExitCodeThread( thread->handle, &thread_exit_code );
      if( thread_exit_code != STILL_ACTIVE )
        {
          TRACE_RETURN( " thread: terminated prematurely." );
          return false;
        }
      ( *SwitchToThread )();
    } while( thread->active == false );
  
  SetLastError( 0 );
  BOOL ret = ( *PostThreadMessageW )( thread->id, 0xFFFF, 0, 0 );
  DWORD gle = GetLastError();
  
  if( !ret || gle )
    {
      TRACE_MSG( " thread: PostThreadMessage test failed." );
      TRACE_MSG( " thread: PostThreadMessage returned: " << ret );
      TRACE_MSG( " thread: GetLastError returned: " << gle );
      TRACE_EXIT();
      return false;
    }

  TRACE_EXIT();
  return true;
}


void W32LowLevelMonitor::terminate()
{
  if( singleton != this )
      return;
  
  unhook();
  
  terminate_thread( callback );
  terminate_thread( dispatch );
}


void W32LowLevelMonitor::terminate_thread( thread_struct *thread )
{
  thread->active = false;
  
  if( thread->id )
      ( *PostThreadMessageW )( thread->id, WM_QUIT, 0, 0 );
  
  wait_for_thread_to_exit( thread );
  
  if( thread->handle != NULL )
    {
      CloseHandle( thread->handle );
      thread->handle = NULL;
    }
  thread->id = 0;
}


void W32LowLevelMonitor::wait_for_thread_to_exit( thread_struct *thread )
{
  DWORD thread_exit_code = 0;
  
  do 
    {
      ( *SwitchToThread )();
      GetExitCodeThread( thread->handle, &thread_exit_code );
    } while( thread_exit_code == STILL_ACTIVE );
}


void W32LowLevelMonitor::unhook()
{
  UnhookWindowsHookEx( k_hook );
  k_hook = NULL;
  
  UnhookWindowsHookEx( m_hook );
  m_hook = NULL;
}


DWORD WINAPI W32LowLevelMonitor::thread_Dispatch( LPVOID lpParam )
{
  W32LowLevelMonitor *pThis = (W32LowLevelMonitor *) lpParam;
  return pThis->dispatch_thread();
}


DWORD W32LowLevelMonitor::dispatch_thread()
{
  dispatch->active = false;
  
  bool ret = 0;
  MSG msg;
  
  // It's good practice to force creation of the thread 
  // message queue before setting active.
  ( *PeekMessageW )( &msg, NULL, WM_USER, WM_USER, PM_NOREMOVE );
  dispatch->active = true;
  
  while( ret = ( *GetMessageW )( &msg, NULL, 0, 0 ) > 0  && dispatch->active )
    {
      msg.message &= 0xFFFF;
      if( msg.message > WM_APP)
      // mouse notification
        {
          switch( msg.message - WM_APP )
            {
              // msg.wParam == x coord
              // msg.lParam == y coord
              case WM_MOUSEMOVE:
                  fire_mouse( msg.wParam, msg.lParam, 0 );
                  break;
              
              case WM_MOUSEWHEEL:
              case WM_MOUSEHWHEEL:
                  fire_mouse( msg.wParam, msg.lParam, 1 );
                  break;
              
              case WM_LBUTTONDOWN:
              case WM_MBUTTONDOWN:
              case WM_RBUTTONDOWN:
              case WM_XBUTTONDOWN:
                  fire_button( 0, true );
                  break;
              
              case WM_LBUTTONUP:
              case WM_MBUTTONUP:
              case WM_RBUTTONUP:
              case WM_XBUTTONUP:
                  fire_button( 0, false );
                  break;
            }
        }
      else if( msg.message == WM_APP )
      // keyboard notification
        {
          // kb->flags == msg.wParam
          if( msg.wParam & 0x00000080 )
          // Transition state: key released
              fire_keyboard( 0, 0 );
          else
          // Transition state: key pressed
              fire_action();
        }
    }
  
  dispatch->active = false;
  
  // Always return a value != STILL_ACTIVE (259)
  return (DWORD) ret;
}


DWORD WINAPI W32LowLevelMonitor::thread_Callback( LPVOID lpParam )
{
  W32LowLevelMonitor *pThis = (W32LowLevelMonitor *) lpParam;
  return pThis->time_critical_callback_thread();
}


DWORD W32LowLevelMonitor::time_critical_callback_thread()
{
  callback->active = false;
  
  int i = 0;
  HANDLE handle = GetCurrentThread();
  
  // An attempt to set the thread priority to 
  // THREAD_PRIORITY_TIME_CRITICAL.
  // Try for several seconds.
  while( SetThreadPriority( handle, 15 ) == 0 )
    {
      if( ++i > 100 )
          return (DWORD) 0;
      else
      //Give up our time slice, try next schedule.
          ( *SwitchToThread )();
    }
  /*
  Do not double check using GetThreadPriority.
  It's possible, throughout this thread's lifetime, that 
  an administrative application could change the priority.
  */
  
  /* MSDN:
  The system creates a thread's message queue when the thread 
  makes its first call to one of the User or GDI functions. 
  */
  unhook(); // thread message queue created here
  
  k_hook = 
      ( *SetWindowsHookExW )( WH_KEYBOARD_LL,  &k_hook_callback, process_handle, 0 );
  m_hook = 
      ( *SetWindowsHookExW )( WH_MOUSE_LL,  &m_hook_callback, process_handle, 0 );
  
  if( !k_hook || !m_hook )
    {
      unhook();
      return (DWORD) 0;
    }
  
  
  MSG msg;
  
  // It's good practice to force creation of the thread 
  // message queue before setting active.
  ( *PeekMessageW )( &msg, NULL, WM_USER, WM_USER, PM_NOREMOVE );
  callback->active = true;
  
  // Message loop. As noted, a hook is called in the 
  // context of the thread that installed it. i.e. this one.
  while( ( *GetMessageW )( &msg, NULL, 0, 0 ) && callback->active )
  ;
  
  unhook();
  callback->active = false;
  
  // Always return a value != STILL_ACTIVE (259)
  return (DWORD) 1;
}


LRESULT CALLBACK W32LowLevelMonitor::k_hook_callback( 
    int nCode, WPARAM wParam, LPARAM lParam )
{
  DWORD flags = ( (_KBDLLHOOKSTRUCT *) lParam )->flags;
  
  if( !nCode && !( flags & LLKHF_INJECTED ) )
  // If there is an event, and it's not injected, notify.
    {
      ( *PostThreadMessageW )
        ( 
          dispatch->id, //idThread
          WM_APP, //Msg
          (WPARAM) flags, //wParam
          (LPARAM) 0 //lParam
        );
    }
  
  return CallNextHookEx( (HHOOK) 0, nCode, wParam, lParam );
}


LRESULT CALLBACK W32LowLevelMonitor::m_hook_callback( 
    int nCode, WPARAM wParam, LPARAM lParam )
{
  DWORD flags = ( (_MSLLHOOKSTRUCT *) lParam )->flags;
  
  if( !nCode && !( flags & LLMHF_INJECTED ) )
  // If there is an event, and it's not injected, notify.
    {
      ( *PostThreadMessageW )
        (
          dispatch->id, //idThread
          WM_APP + (DWORD) wParam, //Msg
          (WPARAM) ( (_MSLLHOOKSTRUCT *) lParam )->pt.x, //wParam
          (LPARAM) ( (_MSLLHOOKSTRUCT *) lParam )->pt.y //lParam
        );
    }
  
  return CallNextHookEx( (HHOOK) 0, nCode, wParam, lParam );
}

//POINTTOPOINTS( ( (MSLLHOOKSTRUCT *) lParam )->pt )