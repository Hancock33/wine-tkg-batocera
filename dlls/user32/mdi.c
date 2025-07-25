/* MDI.C
 *
 * Copyright 1994, Bob Amstadt
 *           1995,1996 Alex Korobka
 * Copyright 2018 Katayama Hirofumi MZ
 *
 * This file contains routines to support MDI (Multiple Document
 * Interface) features .
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Notes: Fairly complete implementation.
 *        Also, Excel and WinWord do _not_ use MDI so if you're trying
 *	  to fix them look elsewhere.
 *
 * Notes on how the "More Windows..." is implemented:
 *
 *      When we have more than 9 opened windows, a "More Windows..."
 *      option appears in the "Windows" menu. Each child window has
 *      a WND* associated with it, accessible via the children list of
 *      the parent window. This WND* has a wIDmenu member, which reflects
 *      the position of the child in the window list. For example, with
 *      9 child windows, we could have the following pattern:
 *
 *
 *
 *                Name of the child window    pWndChild->wIDmenu
 *                     Doc1                       5000
 *                     Doc2                       5001
 *                     Doc3                       5002
 *                     Doc4                       5003
 *                     Doc5                       5004
 *                     Doc6                       5005
 *                     Doc7                       5006
 *                     Doc8                       5007
 *                     Doc9                       5008
 *
 *
 *       The "Windows" menu, as the "More windows..." dialog, are constructed
 *       in this order. If we add a child, we would have the following list:
 *
 *
 *               Name of the child window    pWndChild->wIDmenu
 *                     Doc1                       5000
 *                     Doc2                       5001
 *                     Doc3                       5002
 *                     Doc4                       5003
 *                     Doc5                       5004
 *                     Doc6                       5005
 *                     Doc7                       5006
 *                     Doc8                       5007
 *                     Doc9                       5008
 *                     Doc10                      5009
 *
 *       But only 5000 to 5008 would be displayed in the "Windows" menu. We want
 *       the last created child to be in the menu, so we swap the last child with
 *       the 9th... Doc9 will be accessible via the "More Windows..." option.
 *
 *                     Doc1                       5000
 *                     Doc2                       5001
 *                     Doc3                       5002
 *                     Doc4                       5003
 *                     Doc5                       5004
 *                     Doc6                       5005
 *                     Doc7                       5006
 *                     Doc8                       5007
 *                     Doc9                       5009
 *                     Doc10                      5008
 *
 */

#define OEMRESOURCE

#include <math.h>
#include "user_private.h"
#include "wownt32.h"
#include "controls.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(mdi);

#define MDI_MAXTITLELENGTH      0xa1

#define WM_MDICALCCHILDSCROLL   0x003f /* this is exactly what Windows uses */

/* "More Windows..." definitions */
#define MDI_MOREWINDOWSLIMIT    9       /* after this number of windows, a "More Windows..."
                                           option will appear under the Windows menu */
#define MDI_IDC_LISTBOX         100
#define IDS_MDI_MOREWINDOWS     13

#define MDIF_NEEDUPDATE		0x0001

typedef struct
{
    /* At some points, particularly when switching MDI children, active and
     * maximized MDI children may be not the same window, so we need to track
     * them separately.
     * The only place where we switch to/from maximized state is DefMDIChildProc
     * WM_SIZE/SIZE_MAXIMIZED handler. We get that notification only after the
     * ShowWindow(SW_SHOWMAXIMIZED) request, therefore window is guaranteed to
     * be visible at the time we get the notification, and it's safe to assume
     * that hwndChildMaximized is always visible.
     * If the app plays games with WS_VISIBLE, WS_MAXIMIZE or any other window
     * states it must keep coherency with USER32 on its own. This is true for
     * Windows as well.
     */
    UINT      nActiveChildren;
    HWND      hwndChildMaximized;
    HWND      hwndActiveChild;
    HWND      *child; /* array of tracked children */
    HMENU     hFrameMenu;
    HMENU     hWindowMenu;
    UINT      idFirstChild;
    LPWSTR    frameTitle;
    UINT      nTotalCreated;
    UINT      mdiFlags;
    UINT      sbRecalc;   /* SB_xxx flags for scrollbar fixup */
} MDICLIENTINFO;

static HBITMAP hBmpClose   = 0;

static WCHAR shelltray[] = {'S','h','e','l','l','_','T','r','a','y','W','n','d',0};
static WCHAR progman[] = {'P','r','o','g','m','a','n',0};


/* ----------------- declarations ----------------- */
static void MDI_UpdateFrameText( HWND, HWND, BOOL, LPCWSTR);
static BOOL MDI_AugmentFrameMenu( HWND, HWND );
static BOOL MDI_RestoreFrameMenu( HWND, HWND );
static LONG MDI_ChildActivate( HWND, HWND );
static LRESULT MDI_RefreshMenu(MDICLIENTINFO *);

static HWND MDI_MoreWindowsDialog(HWND);

/* -------- Miscellaneous service functions ----------
 *
 *			MDI_GetChildByID
 */
static HWND MDI_GetChildByID(HWND hwnd, UINT id, MDICLIENTINFO *ci)
{
    int i;

    for (i = 0; ci->nActiveChildren; i++)
    {
        if (GetWindowLongPtrW( ci->child[i], GWLP_ID ) == id)
            return ci->child[i];
    }
    return 0;
}

static void MDI_PostUpdate(HWND hwnd, MDICLIENTINFO* ci, WORD recalc)
{
    if( !(ci->mdiFlags & MDIF_NEEDUPDATE) )
    {
	ci->mdiFlags |= MDIF_NEEDUPDATE;
	PostMessageA( hwnd, WM_MDICALCCHILDSCROLL, 0, 0);
    }
    ci->sbRecalc = recalc;
}


static MDICLIENTINFO *get_client_info( HWND client )
{
    return NtUserGetMDIClientInfo( client );
}

static BOOL is_close_enabled(HWND hwnd, HMENU hSysMenu)
{
    if (GetClassLongW(hwnd, GCL_STYLE) & CS_NOCLOSE) return FALSE;

    if (!hSysMenu) hSysMenu = NtUserGetSystemMenu( hwnd, FALSE );
    if (hSysMenu)
    {
        UINT state = GetMenuState(hSysMenu, SC_CLOSE, MF_BYCOMMAND);
        if (state == 0xFFFFFFFF || (state & (MF_DISABLED | MF_GRAYED)))
            return FALSE;
    }
    return TRUE;
}

/**********************************************************************
 * 			MDI_GetWindow
 *
 * returns "activatable" child different from the current or zero
 */
static HWND MDI_GetWindow(MDICLIENTINFO *clientInfo, HWND hWnd, BOOL bNext,
                            DWORD dwStyleMask )
{
    int i;
    HWND *list;
    HWND last = 0;

    dwStyleMask |= WS_DISABLED | WS_VISIBLE;
    if( !hWnd ) hWnd = clientInfo->hwndActiveChild;

    if (!(list = WIN_ListChildren( GetParent(hWnd) ))) return 0;
    i = 0;
    /* start from next after hWnd */
    while (list[i] && list[i] != hWnd) i++;
    if (list[i]) i++;

    for ( ; list[i]; i++)
    {
        if (GetWindow( list[i], GW_OWNER )) continue;
        if ((GetWindowLongW( list[i], GWL_STYLE ) & dwStyleMask) != WS_VISIBLE) continue;
        last = list[i];
        if (bNext) goto found;
    }
    /* now restart from the beginning */
    for (i = 0; list[i] && list[i] != hWnd; i++)
    {
        if (GetWindow( list[i], GW_OWNER )) continue;
        if ((GetWindowLongW( list[i], GWL_STYLE ) & dwStyleMask) != WS_VISIBLE) continue;
        last = list[i];
        if (bNext) goto found;
    }
 found:
    HeapFree( GetProcessHeap(), 0, list );
    return last;
}

/**********************************************************************
 *			MDI_CalcDefaultChildPos
 *
 *  It seems that the default height is about 2/3 of the client rect
 */
void MDI_CalcDefaultChildPos( HWND hwndClient, INT total, LPPOINT lpPos, INT delta, UINT *id )
{
    INT  nstagger;
    RECT rect;
    INT spacing = GetSystemMetrics(SM_CYCAPTION) + GetSystemMetrics(SM_CYFRAME) - 1;

    if (total < 0) /* we are called from CreateWindow */
    {
        MDICLIENTINFO *ci = get_client_info(hwndClient);
        total = ci->nTotalCreated;
        *id = ci->idFirstChild + ci->nActiveChildren;
        TRACE("MDI child id %04x\n", *id);
    }

    GetClientRect( hwndClient, &rect );
    if( rect.bottom - rect.top - delta >= spacing )
	rect.bottom -= delta;

    nstagger = (rect.bottom - rect.top)/(3 * spacing);
    lpPos[1].x = (rect.right - rect.left - nstagger * spacing);
    lpPos[1].y = (rect.bottom - rect.top - nstagger * spacing);
    lpPos[0].x = lpPos[0].y = spacing * (total%(nstagger+1));
}

/**********************************************************************
 *            MDISetMenu
 */
static LRESULT MDISetMenu( HWND hwnd, HMENU hmenuFrame,
                           HMENU hmenuWindow)
{
    MDICLIENTINFO *ci;
    HWND hwndFrame = GetParent(hwnd);

    TRACE("%p, frame menu %p, window menu %p\n", hwnd, hmenuFrame, hmenuWindow);

    if (hmenuFrame && !IsMenu(hmenuFrame))
    {
	WARN("hmenuFrame is not a menu handle\n");
	return 0L;
    }

    if (hmenuWindow && !IsMenu(hmenuWindow))
    {
	WARN("hmenuWindow is not a menu handle\n");
	return 0L;
    }

    if (!(ci = get_client_info( hwnd ))) return 0;

    TRACE("old frame menu %p, old window menu %p\n", ci->hFrameMenu, ci->hWindowMenu);

    if (hmenuFrame)
    {
        if (hmenuFrame == ci->hFrameMenu) return (LRESULT)hmenuFrame;

        if (ci->hwndChildMaximized)
            MDI_RestoreFrameMenu( hwndFrame, ci->hwndChildMaximized );
    }

    if( hmenuWindow && hmenuWindow != ci->hWindowMenu )
    {
        /* delete menu items from ci->hWindowMenu
         * and add them to hmenuWindow */
        /* Agent newsreader calls this function with  ci->hWindowMenu == NULL */
        if( ci->hWindowMenu && ci->nActiveChildren )
        {
            UINT nActiveChildren_old = ci->nActiveChildren;

            /* Remove all items from old Window menu */
            ci->nActiveChildren = 0;
            MDI_RefreshMenu(ci);

            ci->hWindowMenu = hmenuWindow;

            /* Add items to the new Window menu */
            ci->nActiveChildren = nActiveChildren_old;
            MDI_RefreshMenu(ci);
        }
        else
            ci->hWindowMenu = hmenuWindow;
    }

    if (hmenuFrame)
    {
        NtUserSetMenu(hwndFrame, hmenuFrame);
        if( hmenuFrame != ci->hFrameMenu )
        {
            HMENU oldFrameMenu = ci->hFrameMenu;

            ci->hFrameMenu = hmenuFrame;
            if (ci->hwndChildMaximized)
                MDI_AugmentFrameMenu( hwndFrame, ci->hwndChildMaximized );

            return (LRESULT)oldFrameMenu;
        }
    }

    return 0;
}

/**********************************************************************
 *            MDIRefreshMenu
 */
static LRESULT MDI_RefreshMenu(MDICLIENTINFO *ci)
{
    UINT i, count, visible, id;
    WCHAR buf[MDI_MAXTITLELENGTH];

    TRACE("children %u, window menu %p\n", ci->nActiveChildren, ci->hWindowMenu);

    if (!ci->hWindowMenu)
        return 0;

    if (!IsMenu(ci->hWindowMenu))
    {
        WARN("Window menu handle %p is no longer valid\n", ci->hWindowMenu);
        return 0;
    }

    /* Windows finds the last separator in the menu, and if after it
     * there is a menu item with MDI magic ID removes all existing
     * menu items after it, and then adds visible MDI children.
     */
    count = GetMenuItemCount(ci->hWindowMenu);
    for (i = 0; i < count; i++)
    {
        MENUITEMINFOW mii;

        memset(&mii, 0, sizeof(mii));
        mii.cbSize = sizeof(mii);
        mii.fMask  = MIIM_TYPE;
        if (GetMenuItemInfoW(ci->hWindowMenu, i, TRUE, &mii))
        {
            if (mii.fType & MF_SEPARATOR)
            {
                /* Windows checks only ID of the menu item */
                memset(&mii, 0, sizeof(mii));
                mii.cbSize = sizeof(mii);
                mii.fMask  = MIIM_ID;
                if (GetMenuItemInfoW(ci->hWindowMenu, i + 1, TRUE, &mii))
                {
                    if (mii.wID == ci->idFirstChild)
                    {
                        TRACE("removing %u items including separator\n", count - i);
                        while (NtUserRemoveMenu( ci->hWindowMenu, i, MF_BYPOSITION ))
                            /* nothing */;

                        break;
                    }
                }
            }
        }
    }

    visible = 0;
    for (i = 0; i < ci->nActiveChildren; i++)
    {
        if (GetWindowLongW(ci->child[i], GWL_STYLE) & WS_VISIBLE)
        {
            id = ci->idFirstChild + visible;

            if (visible == MDI_MOREWINDOWSLIMIT)
            {
                LoadStringW(user32_module, IDS_MDI_MOREWINDOWS, buf, ARRAY_SIZE(buf));
                AppendMenuW(ci->hWindowMenu, MF_STRING, id, buf);
                break;
            }

            if (!visible)
                /* Visio expects that separator has id 0 */
                AppendMenuW(ci->hWindowMenu, MF_SEPARATOR, 0, NULL);

            visible++;

            SetWindowLongPtrW(ci->child[i], GWLP_ID, id);

            buf[0] = '&';
            buf[1] = '0' + visible;
            buf[2] = ' ';
            NtUserInternalGetWindowText(ci->child[i], buf + 3, ARRAY_SIZE(buf) - 3);
            TRACE("Adding %p, id %u %s\n", ci->child[i], id, debugstr_w(buf));
            AppendMenuW(ci->hWindowMenu, MF_STRING, id, buf);

            if (ci->child[i] == ci->hwndActiveChild)
                NtUserCheckMenuItem(ci->hWindowMenu, id, MF_CHECKED);
        }
        else
            TRACE("MDI child %p is not visible, skipping\n", ci->child[i]);
    }

    return (LRESULT)ci->hFrameMenu;
}


/* ------------------ MDI child window functions ---------------------- */

/**********************************************************************
 *			MDI_ChildGetMinMaxInfo
 *
 * Note: The rule here is that client rect of the maximized MDI child
 *	 is equal to the client rect of the MDI client window.
 */
static void MDI_ChildGetMinMaxInfo( HWND client, HWND hwnd, MINMAXINFO* lpMinMax )
{
    RECT rect;

    GetClientRect( client, &rect );
    AdjustWindowRectEx( &rect, GetWindowLongW( hwnd, GWL_STYLE ),
                        0, GetWindowLongW( hwnd, GWL_EXSTYLE ));

    lpMinMax->ptMaxSize.x = rect.right -= rect.left;
    lpMinMax->ptMaxSize.y = rect.bottom -= rect.top;

    lpMinMax->ptMaxPosition.x = rect.left;
    lpMinMax->ptMaxPosition.y = rect.top;

    TRACE("max rect %s\n", wine_dbgstr_rect(&rect));
}

/**********************************************************************
 *			MDI_SwitchActiveChild
 *
 * Note: SetWindowPos sends WM_CHILDACTIVATE to the child window that is
 *       being activated
 */
static void MDI_SwitchActiveChild( MDICLIENTINFO *ci, HWND hwndTo, BOOL activate )
{
    HWND hwndPrev;

    hwndPrev = ci->hwndActiveChild;

    TRACE("from %p, to %p\n", hwndPrev, hwndTo);

    if ( hwndTo != hwndPrev )
    {
        BOOL was_zoomed = IsZoomed(hwndPrev);

        if (was_zoomed)
        {
            /* restore old MDI child */
            SendMessageW( hwndPrev, WM_SETREDRAW, FALSE, 0 );
            NtUserShowWindow( hwndPrev, SW_RESTORE );
            SendMessageW( hwndPrev, WM_SETREDRAW, TRUE, 0 );

            /* activate new MDI child */
            NtUserSetWindowPos( hwndTo, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE );
            /* maximize new MDI child */
            NtUserShowWindow( hwndTo, SW_MAXIMIZE );
        }
        /* activate new MDI child */
        NtUserSetWindowPos( hwndTo, HWND_TOP, 0, 0, 0, 0,
                            SWP_NOMOVE | SWP_NOSIZE | (activate ? 0 : SWP_NOACTIVATE) );
    }
}


/**********************************************************************
 *                                      MDIDestroyChild
 */
static LRESULT MDIDestroyChild( HWND client, MDICLIENTINFO *ci,
                                HWND child, BOOL flagDestroy )
{
    UINT i;

    TRACE("# of managed children %u\n", ci->nActiveChildren);

    if( child == ci->hwndActiveChild )
    {
        HWND next = MDI_GetWindow(ci, child, TRUE, 0);
        /* flagDestroy == 0 means we were called from WM_PARENTNOTIFY handler */
        if (flagDestroy && next)
            MDI_SwitchActiveChild(ci, next, TRUE);
        else
        {
            NtUserShowWindow( child, SW_HIDE );
            if (child == ci->hwndChildMaximized)
            {
                HWND frame = GetParent(client);
                MDI_RestoreFrameMenu(frame, child);
                ci->hwndChildMaximized = 0;
                MDI_UpdateFrameText(frame, client, TRUE, NULL);
            }
            if (flagDestroy)
                MDI_ChildActivate(client, 0);
        }
    }

    for (i = 0; i < ci->nActiveChildren; i++)
    {
        if (ci->child[i] == child)
        {
            HWND *new_child = HeapAlloc(GetProcessHeap(), 0, (ci->nActiveChildren - 1) * sizeof(HWND));
            memcpy(new_child, ci->child, i * sizeof(HWND));
            if (i + 1 < ci->nActiveChildren)
                memcpy(new_child + i, ci->child + i + 1, (ci->nActiveChildren - i - 1) * sizeof(HWND));
            HeapFree(GetProcessHeap(), 0, ci->child);
            ci->child = new_child;

            ci->nActiveChildren--;
            break;
        }
    }

    if (flagDestroy)
    {
        SendMessageW(client, WM_MDIREFRESHMENU, 0, 0);
        MDI_PostUpdate(GetParent(child), ci, SB_BOTH+1);
        NtUserDestroyWindow(child);
    }

    TRACE("child destroyed - %p\n", child);
    return 0;
}


/**********************************************************************
 *					MDI_ChildActivate
 *
 * Called in response to WM_CHILDACTIVATE, or when last MDI child
 * is being deactivated.
 */
static LONG MDI_ChildActivate( HWND client, HWND child )
{
    MDICLIENTINFO *clientInfo;
    HWND prevActiveWnd, frame;
    BOOL isActiveFrameWnd;

    clientInfo = get_client_info( client );

    if (clientInfo->hwndActiveChild == child) return 0;

    TRACE("%p\n", child);

    frame = GetParent(client);
    isActiveFrameWnd = (GetActiveWindow() == frame);
    prevActiveWnd = clientInfo->hwndActiveChild;

    /* deactivate prev. active child */
    if(prevActiveWnd)
    {
        SendMessageW( prevActiveWnd, WM_NCACTIVATE, FALSE, 0L );
        SendMessageW( prevActiveWnd, WM_MDIACTIVATE, (WPARAM)prevActiveWnd, (LPARAM)child);
    }

    MDI_SwitchActiveChild( clientInfo, child, FALSE );
    clientInfo->hwndActiveChild = child;

    MDI_RefreshMenu(clientInfo);

    if( isActiveFrameWnd )
    {
        SendMessageW( child, WM_NCACTIVATE, TRUE, 0L);
        /* Let the client window manage focus for children, but if the focus
         * is already on the client (for instance this is the 1st child) then
         * SetFocus won't work. It appears that Windows sends WM_SETFOCUS
         * manually in this case.
         */
        if (NtUserSetFocus(client) == client)
            SendMessageW( client, WM_SETFOCUS, (WPARAM)client, 0 );
    }

    SendMessageW( child, WM_MDIACTIVATE, (WPARAM)prevActiveWnd, (LPARAM)child );
    return TRUE;
}

/* -------------------- MDI client window functions ------------------- */

/**********************************************************************
 *				CreateMDIMenuBitmap
 */
static HBITMAP CreateMDIMenuBitmap(void)
{
 HDC 		hDCSrc  = CreateCompatibleDC(0);
 HDC		hDCDest	= CreateCompatibleDC(hDCSrc);
 HBITMAP	hbClose = LoadBitmapW(0, MAKEINTRESOURCEW(OBM_OLD_CLOSE) );
 HBITMAP	hbCopy;
 HBITMAP	hobjSrc, hobjDest;

 hobjSrc = SelectObject(hDCSrc, hbClose);
 hbCopy = CreateCompatibleBitmap(hDCSrc,GetSystemMetrics(SM_CXSIZE),GetSystemMetrics(SM_CYSIZE));
 hobjDest = SelectObject(hDCDest, hbCopy);

 BitBlt(hDCDest, 0, 0, GetSystemMetrics(SM_CXSIZE), GetSystemMetrics(SM_CYSIZE),
          hDCSrc, GetSystemMetrics(SM_CXSIZE), 0, SRCCOPY);

 SelectObject(hDCSrc, hobjSrc);
 DeleteObject(hbClose);
 DeleteDC(hDCSrc);

 hobjSrc = SelectObject( hDCDest, GetStockObject(BLACK_PEN) );

 MoveToEx( hDCDest, GetSystemMetrics(SM_CXSIZE) - 1, 0, NULL );
 LineTo( hDCDest, GetSystemMetrics(SM_CXSIZE) - 1, GetSystemMetrics(SM_CYSIZE) - 1);

 SelectObject(hDCDest, hobjSrc );
 SelectObject(hDCDest, hobjDest);
 DeleteDC(hDCDest);

 return hbCopy;
}

/**********************************************************************
 *				MDICascade
 */
static LONG MDICascade( HWND client, MDICLIENTINFO *ci )
{
    HWND *win_array;
    BOOL has_icons = FALSE;
    int i, total;

    if (ci->hwndChildMaximized)
        SendMessageW(client, WM_MDIRESTORE, (WPARAM)ci->hwndChildMaximized, 0);

    if (ci->nActiveChildren == 0) return 0;

    if (!(win_array = WIN_ListChildren( client ))) return 0;

    /* remove all the windows we don't want */
    for (i = total = 0; win_array[i]; i++)
    {
        if (!IsWindowVisible( win_array[i] )) continue;
        if (GetWindow( win_array[i], GW_OWNER )) continue; /* skip owned windows */
        if (IsIconic( win_array[i] ))
        {
            has_icons = TRUE;
            continue;
        }
        win_array[total++] = win_array[i];
    }
    win_array[total] = 0;

    if (total)
    {
        INT delta = 0, n = 0, i;
        POINT pos[2];
        if (has_icons) delta = GetSystemMetrics(SM_CYICONSPACING) + GetSystemMetrics(SM_CYICON);

        /* walk the list (backwards) and move windows */
        for (i = total - 1; i >= 0; i--)
        {
            LONG style;
            LONG posOptions = SWP_DRAWFRAME | SWP_NOACTIVATE | SWP_NOZORDER;

            MDI_CalcDefaultChildPos(client, n++, pos, delta, NULL);
            TRACE("move %p to (%ld,%ld) size [%ld,%ld]\n",
                  win_array[i], pos[0].x, pos[0].y, pos[1].x, pos[1].y);
            style = GetWindowLongW(win_array[i], GWL_STYLE);

            if (!(style & WS_SIZEBOX)) posOptions |= SWP_NOSIZE;
            NtUserSetWindowPos( win_array[i], 0, pos[0].x, pos[0].y, pos[1].x, pos[1].y, posOptions );
        }
    }
    HeapFree( GetProcessHeap(), 0, win_array );

    if (has_icons) NtUserArrangeIconicWindows( client );
    return 0;
}

/**********************************************************************
 *					MDITile
 */
static void MDITile( HWND client, MDICLIENTINFO *ci, WPARAM wParam )
{
    HWND *win_array;
    int i, total;
    BOOL has_icons = FALSE;

    if (ci->hwndChildMaximized)
        SendMessageW(client, WM_MDIRESTORE, (WPARAM)ci->hwndChildMaximized, 0);

    if (ci->nActiveChildren == 0) return;

    if (!(win_array = WIN_ListChildren( client ))) return;

    /* remove all the windows we don't want */
    for (i = total = 0; win_array[i]; i++)
    {
        if (!IsWindowVisible( win_array[i] )) continue;
        if (GetWindow( win_array[i], GW_OWNER )) continue; /* skip owned windows (icon titles) */
        if (IsIconic( win_array[i] ))
        {
            has_icons = TRUE;
            continue;
        }
        if ((wParam & MDITILE_SKIPDISABLED) && !IsWindowEnabled( win_array[i] )) continue;
        win_array[total++] = win_array[i];
    }
    win_array[total] = 0;

    TRACE("%u windows to tile\n", total);

    if (total)
    {
        HWND *pWnd = win_array;
        RECT rect;
        int x, y, xsize, ysize;
        int rows, columns, r, c, i;

        GetClientRect(client,&rect);
        rows    = (int) sqrt((double)total);
        columns = total / rows;

        if( wParam & MDITILE_HORIZONTAL )  /* version >= 3.1 */
        {
            i = rows;
            rows = columns;  /* exchange r and c */
            columns = i;
        }

        if (has_icons)
        {
            y = rect.bottom - 2 * GetSystemMetrics(SM_CYICONSPACING) - GetSystemMetrics(SM_CYICON);
            rect.bottom = ( y - GetSystemMetrics(SM_CYICON) < rect.top )? rect.bottom: y;
        }

        ysize   = rect.bottom / rows;
        xsize   = rect.right  / columns;

        for (x = i = 0, c = 1; c <= columns && *pWnd; c++)
        {
            if (c == columns)
            {
                rows  = total - i;
                ysize = rect.bottom / rows;
            }

            y = 0;
            for (r = 1; r <= rows && *pWnd; r++, i++)
            {
                LONG posOptions = SWP_DRAWFRAME | SWP_NOACTIVATE | SWP_NOZORDER;
                LONG style = GetWindowLongW(win_array[i], GWL_STYLE);
                if (!(style & WS_SIZEBOX)) posOptions |= SWP_NOSIZE;

                NtUserSetWindowPos( *pWnd, 0, x, y, xsize, ysize, posOptions );
                y += ysize;
                pWnd++;
            }
            x += xsize;
        }
    }
    HeapFree( GetProcessHeap(), 0, win_array );
    if (has_icons) NtUserArrangeIconicWindows( client );
}

/* ----------------------- Frame window ---------------------------- */


/**********************************************************************
 *					MDI_AugmentFrameMenu
 */
static BOOL MDI_AugmentFrameMenu( HWND frame, HWND hChild )
{
    HMENU menu = GetMenu( frame );
    HMENU  	hSysPopup;
    HBITMAP hSysMenuBitmap = 0;
    HICON hIcon;

    TRACE("frame %p,child %p\n",frame,hChild);

    if (!menu) return FALSE;

    /* create a copy of sysmenu popup and insert it into frame menu bar */
    if (!(hSysPopup = NtUserGetSystemMenu( hChild, FALSE )))
    {
        TRACE("child %p doesn't have a system menu\n", hChild);
        return FALSE;
    }

    AppendMenuW(menu, MF_HELP | MF_BITMAP,
                SC_CLOSE, is_close_enabled(hChild, hSysPopup) ?
                (LPCWSTR)HBMMENU_MBAR_CLOSE : (LPCWSTR)HBMMENU_MBAR_CLOSE_D );
    AppendMenuW(menu, MF_HELP | MF_BITMAP,
                SC_RESTORE, (LPCWSTR)HBMMENU_MBAR_RESTORE );
    AppendMenuW(menu, MF_HELP | MF_BITMAP,
                SC_MINIMIZE, (LPCWSTR)HBMMENU_MBAR_MINIMIZE ) ;

    /* The system menu is replaced by the child icon */
    hIcon = (HICON)SendMessageW(hChild, WM_GETICON, ICON_SMALL, 0);
    if (!hIcon)
        hIcon = (HICON)GetClassLongPtrW(hChild, GCLP_HICONSM);
    if (!hIcon)
        hIcon = (HICON)SendMessageW(hChild, WM_GETICON, ICON_BIG, 0);
    if (!hIcon)
        hIcon = (HICON)GetClassLongPtrW(hChild, GCLP_HICON);
    if (!hIcon)
        hIcon = LoadImageW(0, (LPWSTR)IDI_WINLOGO, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR | LR_SHARED);
    if (hIcon)
    {
      HDC hMemDC;
      HBITMAP hBitmap, hOldBitmap;
      HDC hdc = NtUserGetDC(hChild);

      if (hdc)
      {
        int cx, cy;
        cx = GetSystemMetrics(SM_CXSMICON);
        cy = GetSystemMetrics(SM_CYSMICON);
        hMemDC = CreateCompatibleDC(hdc);
        hBitmap = CreateCompatibleBitmap(hdc, cx, cy);
        hOldBitmap = SelectObject(hMemDC, hBitmap);
        SetMapMode(hMemDC, MM_TEXT);
        NtUserDrawIconEx(hMemDC, 0, 0, hIcon, cx, cy, 0, GetSysColorBrush(COLOR_MENU), DI_NORMAL);
        SelectObject (hMemDC, hOldBitmap);
        DeleteDC(hMemDC);
        NtUserReleaseDC( hChild, hdc );
        hSysMenuBitmap = hBitmap;
      }
    }

    if( !InsertMenuA(menu,0,MF_BYPOSITION | MF_BITMAP | MF_POPUP,
                     (UINT_PTR)hSysPopup, (LPSTR)hSysMenuBitmap))
    {
        TRACE("not inserted\n");
        NtUserDestroyMenu( hSysPopup );
        return FALSE;
    }

    NtUserEnableMenuItem(hSysPopup, SC_SIZE, MF_BYCOMMAND | MF_GRAYED);
    NtUserEnableMenuItem(hSysPopup, SC_MOVE, MF_BYCOMMAND | MF_GRAYED);
    NtUserEnableMenuItem(hSysPopup, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED);
    NtUserSetMenuDefaultItem(hSysPopup, SC_CLOSE, FALSE);

    /* redraw menu */
    NtUserDrawMenuBar(frame);

    return TRUE;
}

/**********************************************************************
 *					MDI_RestoreFrameMenu
 */
static BOOL MDI_RestoreFrameMenu( HWND frame, HWND hChild )
{
    MENUITEMINFOW menuInfo;
    HMENU menu = GetMenu( frame );
    INT nItems;
    UINT iId;

    TRACE("frame %p, child %p\n", frame, hChild);

    if (!menu) return FALSE;

    /* if there is no system buttons then nothing to do */
    nItems = GetMenuItemCount(menu) - 1;
    iId = GetMenuItemID(menu, nItems);
    if ( !(iId == SC_RESTORE || iId == SC_CLOSE) )
        return FALSE;

    /*
     * Remove the system menu, If that menu is the icon of the window
     * as it is in win95, we have to delete the bitmap.
     */
    memset(&menuInfo, 0, sizeof(menuInfo));
    menuInfo.cbSize = sizeof(menuInfo);
    menuInfo.fMask  = MIIM_DATA | MIIM_TYPE;

    GetMenuItemInfoW(menu,
		     0,
		     TRUE,
		     &menuInfo);

    NtUserRemoveMenu( menu, 0, MF_BYPOSITION );

    if ( (menuInfo.fType & MFT_BITMAP)           &&
	 (LOWORD(menuInfo.dwTypeData)!=0)        &&
	 (LOWORD(menuInfo.dwTypeData)!=HBITMAP_16(hBmpClose)) )
    {
        DeleteObject(HBITMAP_32(LOWORD(menuInfo.dwTypeData)));
    }

    /* close */
    NtUserDeleteMenu( menu, SC_CLOSE, MF_BYCOMMAND );
    /* restore */
    NtUserDeleteMenu( menu, SC_RESTORE, MF_BYCOMMAND );
    /* minimize */
    NtUserDeleteMenu( menu, SC_MINIMIZE, MF_BYCOMMAND );

    NtUserDrawMenuBar(frame);

    return TRUE;
}


/**********************************************************************
 *				        MDI_UpdateFrameText
 *
 * used when child window is maximized/restored
 *
 * Note: lpTitle can be NULL
 */
static void MDI_UpdateFrameText( HWND frame, HWND hClient, BOOL repaint, LPCWSTR lpTitle )
{
    WCHAR   lpBuffer[MDI_MAXTITLELENGTH+1];
    MDICLIENTINFO *ci = get_client_info( hClient );

    TRACE("frameText %s\n", debugstr_w(lpTitle));

    if (!ci) return;

    if (!lpTitle && !ci->frameTitle)  /* first time around, get title from the frame window */
    {
        GetWindowTextW( frame, lpBuffer, ARRAY_SIZE( lpBuffer ));
        lpTitle = lpBuffer;
    }

    /* store new "default" title if lpTitle is not NULL */
    if (lpTitle)
    {
	HeapFree( GetProcessHeap(), 0, ci->frameTitle );
	if ((ci->frameTitle = HeapAlloc( GetProcessHeap(), 0, (lstrlenW(lpTitle)+1)*sizeof(WCHAR))))
            lstrcpyW( ci->frameTitle, lpTitle );
    }

    if (ci->frameTitle)
    {
	if (ci->hwndChildMaximized)
	{
	    /* combine frame title and child title if possible */
	    int	i_frame_text_length = lstrlenW(ci->frameTitle);

	    lstrcpynW( lpBuffer, ci->frameTitle, MDI_MAXTITLELENGTH);

	    if( i_frame_text_length + 6 < MDI_MAXTITLELENGTH )
            {
		lstrcatW( lpBuffer, L" - [" );
                if (GetWindowTextW( ci->hwndActiveChild, lpBuffer + i_frame_text_length + 4,
                                    MDI_MAXTITLELENGTH - i_frame_text_length - 5 ))
                    lstrcatW( lpBuffer, L"]" );
                else
                    lpBuffer[i_frame_text_length] = 0;  /* remove bracket */
            }
	}
	else
	{
            lstrcpynW(lpBuffer, ci->frameTitle, MDI_MAXTITLELENGTH+1 );
	}
    }
    else
	lpBuffer[0] = '\0';

    DefWindowProcW( frame, WM_SETTEXT, 0, (LPARAM)lpBuffer );

    if (repaint)
        NtUserSetWindowPos( frame, 0,0,0,0,0, SWP_FRAMECHANGED |
                            SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER );
}


/* ----------------------------- Interface ---------------------------- */


/**********************************************************************
 *		MDIClientWndProc_common
 */
LRESULT MDIClientWndProc_common( HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, BOOL unicode )
{
    MDICLIENTINFO *ci;

    TRACE("%p %04x (%s) %08Ix %08Ix\n", hwnd, message, SPY_GetMsgName(message, hwnd), wParam, lParam);

    if (!(ci = get_client_info( hwnd )))
    {
        if (message == WM_NCCREATE)
        {
            if (!(ci = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ci) ))) return 0;
            NtUserSetMDIClientInfo( hwnd, ci );
        }
        return unicode ? DefWindowProcW( hwnd, message, wParam, lParam ) :
                         DefWindowProcA( hwnd, message, wParam, lParam );
    }

    switch (message)
    {
      case WM_CREATE:
      {
          /* Since we are using only cs->lpCreateParams, we can safely
           * cast to LPCREATESTRUCTA here */
        LPCREATESTRUCTA cs = (LPCREATESTRUCTA)lParam;
        LPCLIENTCREATESTRUCT ccs = cs->lpCreateParams;

        ci->hWindowMenu		= ccs->hWindowMenu;
        ci->idFirstChild	= ccs->idFirstChild;
        ci->hwndChildMaximized  = 0;
        ci->child = NULL;
	ci->nActiveChildren	= 0;
	ci->nTotalCreated	= 0;
	ci->frameTitle		= NULL;
	ci->mdiFlags		= 0;
        ci->hFrameMenu = GetMenu(cs->hwndParent);

	if (!hBmpClose) hBmpClose = CreateMDIMenuBitmap();

        TRACE("Client created: hwnd %p, Window menu %p, idFirst = %04x\n",
              hwnd, ci->hWindowMenu, ci->idFirstChild );
        return 0;
      }

      case WM_DESTROY:
      {
          if( ci->hwndChildMaximized )
              MDI_RestoreFrameMenu(GetParent(hwnd), ci->hwndChildMaximized);

          ci->nActiveChildren = 0;
          MDI_RefreshMenu(ci);

          HeapFree( GetProcessHeap(), 0, ci->child );
          HeapFree( GetProcessHeap(), 0, ci->frameTitle );
          HeapFree( GetProcessHeap(), 0, ci );
          return 0;
      }

      case WM_MDIACTIVATE:
      {
        if( ci->hwndActiveChild != (HWND)wParam )
	    NtUserSetWindowPos( (HWND)wParam, 0,0,0,0,0, SWP_NOSIZE | SWP_NOMOVE );
        return 0;
      }

      case WM_MDICASCADE:
        return MDICascade(hwnd, ci);

      case WM_MDICREATE:
        if (lParam)
        {
            HWND child;

            if (unicode)
            {
                MDICREATESTRUCTW *csW = (MDICREATESTRUCTW *)lParam;
                child = CreateWindowExW(WS_EX_MDICHILD, csW->szClass,
                                            csW->szTitle, csW->style,
                                            csW->x, csW->y, csW->cx, csW->cy,
                                            hwnd, 0, csW->hOwner,
                                            (LPVOID)csW->lParam);
            }
            else
            {
                MDICREATESTRUCTA *csA = (MDICREATESTRUCTA *)lParam;
                child = CreateWindowExA(WS_EX_MDICHILD, csA->szClass,
                                            csA->szTitle, csA->style,
                                            csA->x, csA->y, csA->cx, csA->cy,
                                            hwnd, 0, csA->hOwner,
                                            (LPVOID)csA->lParam);
            }
            return (LRESULT)child;
        }
        return 0;

      case WM_MDIDESTROY:
          return MDIDestroyChild( hwnd, ci, WIN_GetFullHandle( (HWND)wParam ), TRUE );

      case WM_MDIGETACTIVE:
          if (lParam) *(BOOL *)lParam = IsZoomed(ci->hwndActiveChild);
          return (LRESULT)ci->hwndActiveChild;

      case WM_MDIICONARRANGE:
	ci->mdiFlags |= MDIF_NEEDUPDATE;
        NtUserArrangeIconicWindows( hwnd );
	ci->sbRecalc = SB_BOTH+1;
        SendMessageW( hwnd, WM_MDICALCCHILDSCROLL, 0, 0 );
        return 0;

      case WM_MDIMAXIMIZE:
	NtUserShowWindow( (HWND)wParam, SW_MAXIMIZE );
        return 0;

      case WM_MDINEXT: /* lParam != 0 means previous window */
      {
        HWND hwnd = wParam ? WIN_GetFullHandle((HWND)wParam) : ci->hwndActiveChild;
        HWND next = MDI_GetWindow( ci, hwnd, !lParam, 0 );
        MDI_SwitchActiveChild( ci, next, TRUE );
        if (!lParam)
            NtUserSetWindowPos( hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE );
	break;
      }

      case WM_MDIRESTORE:
        NtUserShowWindow( (HWND)wParam, SW_SHOWNORMAL );
        return 0;

      case WM_MDISETMENU:
          return MDISetMenu( hwnd, (HMENU)wParam, (HMENU)lParam );

      case WM_MDIREFRESHMENU:
          return MDI_RefreshMenu( ci );

      case WM_MDITILE:
	ci->mdiFlags |= MDIF_NEEDUPDATE;
        NtUserShowScrollBar( hwnd, SB_BOTH, FALSE );
        MDITile( hwnd, ci, wParam );
        ci->mdiFlags &= ~MDIF_NEEDUPDATE;
        return 0;

      case WM_VSCROLL:
      case WM_HSCROLL:
	ci->mdiFlags |= MDIF_NEEDUPDATE;
        ScrollChildren( hwnd, message, wParam, lParam );
	ci->mdiFlags &= ~MDIF_NEEDUPDATE;
        return 0;

      case WM_SETFOCUS:
          if (ci->hwndActiveChild && !IsIconic( ci->hwndActiveChild ))
              NtUserSetFocus( ci->hwndActiveChild );
          return 0;

      case WM_NCACTIVATE:
        if( ci->hwndActiveChild )
            SendMessageW(ci->hwndActiveChild, message, wParam, lParam);
	break;

      case WM_PARENTNOTIFY:
        switch (LOWORD(wParam))
        {
        case WM_CREATE:
            if (GetWindowLongW((HWND)lParam, GWL_EXSTYLE) & WS_EX_MDICHILD)
            {
                ci->nTotalCreated++;
                ci->nActiveChildren++;

                if (!ci->child)
                    ci->child = HeapAlloc(GetProcessHeap(), 0, sizeof(HWND));
                else
                    ci->child = HeapReAlloc(GetProcessHeap(), 0, ci->child, sizeof(HWND) * ci->nActiveChildren);

                TRACE("Adding MDI child %p, # of children %d\n",
                      (HWND)lParam, ci->nActiveChildren);

                ci->child[ci->nActiveChildren - 1] = (HWND)lParam;
            }
            break;

        case WM_LBUTTONDOWN:
            {
            HWND child;
            POINT pt;
            pt.x = (short)LOWORD(lParam);
            pt.y = (short)HIWORD(lParam);
            child = ChildWindowFromPoint(hwnd, pt);

            TRACE("notification from %p (%li,%li)\n",child,pt.x,pt.y);

            if (child && child != hwnd && child != ci->hwndActiveChild)
                NtUserSetWindowPos( child, 0,0,0,0,0, SWP_NOSIZE | SWP_NOMOVE );
            break;
            }

        case WM_DESTROY:
            return MDIDestroyChild( hwnd, ci, WIN_GetFullHandle( (HWND)lParam ), FALSE );
        }
        return 0;

      case WM_SIZE:
        if( ci->hwndActiveChild && IsZoomed(ci->hwndActiveChild) )
	{
	    RECT	rect;

            SetRect(&rect, 0, 0, LOWORD(lParam), HIWORD(lParam));
	    AdjustWindowRectEx(&rect, GetWindowLongA(ci->hwndActiveChild, GWL_STYLE),
                               0, GetWindowLongA(ci->hwndActiveChild, GWL_EXSTYLE) );
	    NtUserMoveWindow( ci->hwndActiveChild, rect.left, rect.top,
                              rect.right - rect.left, rect.bottom - rect.top, 1 );
	}
	else
            MDI_PostUpdate(hwnd, ci, SB_BOTH+1);

	break;

      case WM_MDICALCCHILDSCROLL:
	if( (ci->mdiFlags & MDIF_NEEDUPDATE) && ci->sbRecalc )
	{
            CalcChildScroll(hwnd, ci->sbRecalc-1);
	    ci->sbRecalc = 0;
	    ci->mdiFlags &= ~MDIF_NEEDUPDATE;
	}
        return 0;
    }
    return unicode ? DefWindowProcW( hwnd, message, wParam, lParam ) :
                     DefWindowProcA( hwnd, message, wParam, lParam );
}

/***********************************************************************
 *		DefFrameProcA (USER32.@)
 */
LRESULT WINAPI DefFrameProcA( HWND hwnd, HWND hwndMDIClient,
                                UINT message, WPARAM wParam, LPARAM lParam)
{
    if (hwndMDIClient)
    {
	switch (message)
	{
        case WM_SETTEXT:
            {
                DWORD len = MultiByteToWideChar( CP_ACP, 0, (LPSTR)lParam, -1, NULL, 0 );
                LPWSTR text = HeapAlloc( GetProcessHeap(), 0, len * sizeof(WCHAR) );
                MultiByteToWideChar( CP_ACP, 0, (LPSTR)lParam, -1, text, len );
                MDI_UpdateFrameText( hwnd, hwndMDIClient, FALSE, text );
                HeapFree( GetProcessHeap(), 0, text );
            }
            return 1; /* success. FIXME: check text length */

        case WM_COMMAND:
        case WM_NCACTIVATE:
        case WM_NEXTMENU:
        case WM_SETFOCUS:
        case WM_SIZE:
            return DefFrameProcW( hwnd, hwndMDIClient, message, wParam, lParam );
        }
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}


/***********************************************************************
 *		DefFrameProcW (USER32.@)
 */
LRESULT WINAPI DefFrameProcW( HWND hwnd, HWND hwndMDIClient,
                                UINT message, WPARAM wParam, LPARAM lParam)
{
    MDICLIENTINFO *ci = get_client_info( hwndMDIClient );

    TRACE("%p %p %04x (%s) %08Ix %08Ix\n", hwnd, hwndMDIClient, message, SPY_GetMsgName(message, hwnd), wParam, lParam);

    if (ci)
    {
	switch (message)
	{
        case WM_COMMAND:
            {
                WORD id = LOWORD(wParam);
                /* check for possible syscommands for maximized MDI child */
                if (id <  ci->idFirstChild || id >= ci->idFirstChild + ci->nActiveChildren)
                {
                    if( (id - 0xf000) & 0xf00f ) break;
                    if( !ci->hwndChildMaximized ) break;
                    switch( id )
                    {
                    case SC_CLOSE:
                        if (!is_close_enabled(ci->hwndActiveChild, 0)) break;
                    case SC_SIZE:
                    case SC_MOVE:
                    case SC_MINIMIZE:
                    case SC_MAXIMIZE:
                    case SC_NEXTWINDOW:
                    case SC_PREVWINDOW:
                    case SC_RESTORE:
                        return SendMessageW( ci->hwndChildMaximized, WM_SYSCOMMAND,
                                             wParam, lParam);
                    }
                }
                else
                {
                    HWND childHwnd;
                    if (id - ci->idFirstChild == MDI_MOREWINDOWSLIMIT)
                        /* User chose "More Windows..." */
                        childHwnd = MDI_MoreWindowsDialog(hwndMDIClient);
                    else
                        /* User chose one of the windows listed in the "Windows" menu */
                        childHwnd = MDI_GetChildByID(hwndMDIClient, id, ci);

                    if( childHwnd )
                        SendMessageW( hwndMDIClient, WM_MDIACTIVATE, (WPARAM)childHwnd, 0 );
                }
            }
            break;

        case WM_NCACTIVATE:
	    SendMessageW(hwndMDIClient, message, wParam, lParam);
	    break;

        case WM_SETTEXT:
            MDI_UpdateFrameText( hwnd, hwndMDIClient, FALSE, (LPWSTR)lParam );
	    return 1; /* success. FIXME: check text length */

        case WM_SETFOCUS:
	    NtUserSetFocus( hwndMDIClient );
	    break;

        case WM_SIZE:
            NtUserMoveWindow( hwndMDIClient, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE );
            break;

        case WM_NEXTMENU:
            {
                MDINEXTMENU *next_menu = (MDINEXTMENU *)lParam;

                if (!IsIconic(hwnd) && ci->hwndActiveChild && !IsZoomed(ci->hwndActiveChild))
                {
                    /* control menu is between the frame system menu and
                     * the first entry of menu bar */
                    if ((wParam == VK_LEFT && GetMenu(hwnd) == next_menu->hmenuIn) ||
                        (wParam == VK_RIGHT && NtUserGetWindowSysSubMenu( hwnd ) == next_menu->hmenuIn))
                    {
                        next_menu->hmenuNext = NtUserGetWindowSysSubMenu( ci->hwndActiveChild );
                        next_menu->hwndNext = ci->hwndActiveChild;
                    }
                }
                return 0;
            }
	}
    }

    return DefWindowProcW( hwnd, message, wParam, lParam );
}

/***********************************************************************
 *		DefMDIChildProcA (USER32.@)
 */
LRESULT WINAPI DefMDIChildProcA( HWND hwnd, UINT message,
                                   WPARAM wParam, LPARAM lParam )
{
    HWND client = GetParent(hwnd);
    MDICLIENTINFO *ci = get_client_info( client );

    TRACE("%p %04x (%s) %08Ix %08Ix\n", hwnd, message, SPY_GetMsgName(message, hwnd), wParam, lParam);

    hwnd = WIN_GetFullHandle( hwnd );
    if (!ci) return DefWindowProcA( hwnd, message, wParam, lParam );

    switch (message)
    {
    case WM_SETTEXT:
	DefWindowProcA(hwnd, message, wParam, lParam);
	if( ci->hwndChildMaximized == hwnd )
	    MDI_UpdateFrameText( GetParent(client), client, TRUE, NULL );
        MDI_RefreshMenu( ci );
        return 1; /* success. FIXME: check text length */

    case WM_GETMINMAXINFO:
    case WM_MENUCHAR:
    case WM_CLOSE:
    case WM_SETFOCUS:
    case WM_CHILDACTIVATE:
    case WM_SYSCOMMAND:
    case WM_SHOWWINDOW:
    case WM_SETVISIBLE:
    case WM_SIZE:
    case WM_NEXTMENU:
    case WM_SYSCHAR:
    case WM_DESTROY:
        return DefMDIChildProcW( hwnd, message, wParam, lParam );
    }
    return DefWindowProcA(hwnd, message, wParam, lParam);
}


/***********************************************************************
 *		DefMDIChildProcW (USER32.@)
 */
LRESULT WINAPI DefMDIChildProcW( HWND hwnd, UINT message,
                                   WPARAM wParam, LPARAM lParam )
{
    HWND client = GetParent(hwnd);
    MDICLIENTINFO *ci = get_client_info( client );

    TRACE("%p %04x (%s) %08Ix %08Ix\n", hwnd, message, SPY_GetMsgName(message, hwnd), wParam, lParam);

    hwnd = WIN_GetFullHandle( hwnd );
    if (!ci) return DefWindowProcW( hwnd, message, wParam, lParam );

    switch (message)
    {
    case WM_SETTEXT:
        DefWindowProcW(hwnd, message, wParam, lParam);
        if( ci->hwndChildMaximized == hwnd )
            MDI_UpdateFrameText( GetParent(client), client, TRUE, NULL );
        MDI_RefreshMenu( ci );
        return 1; /* success. FIXME: check text length */

    case WM_GETMINMAXINFO:
        MDI_ChildGetMinMaxInfo( client, hwnd, (MINMAXINFO *)lParam );
        return 0;

    case WM_MENUCHAR:
        return MAKELRESULT( 0, MNC_CLOSE ); /* MDI children don't have menu bars */

    case WM_CLOSE:
        SendMessageW( client, WM_MDIDESTROY, (WPARAM)hwnd, 0 );
        return 0;

    case WM_SETFOCUS:
        if (ci->hwndActiveChild != hwnd)
            MDI_ChildActivate( client, hwnd );
        break;

    case WM_CHILDACTIVATE:
        if (IsWindowEnabled( hwnd ))
            MDI_ChildActivate( client, hwnd );
        return 0;

    case WM_SYSCOMMAND:
        switch (wParam & 0xfff0)
        {
        case SC_MOVE:
            if( ci->hwndChildMaximized == hwnd )
                return 0;
            break;
        case SC_RESTORE:
        case SC_MINIMIZE:
            break;
        case SC_MAXIMIZE:
            if (ci->hwndChildMaximized == hwnd)
                return SendMessageW( GetParent(client), message, wParam, lParam);
            break;
        case SC_NEXTWINDOW:
            SendMessageW( client, WM_MDINEXT, (WPARAM)ci->hwndActiveChild, 0);
            return 0;
        case SC_PREVWINDOW:
            SendMessageW( client, WM_MDINEXT, (WPARAM)ci->hwndActiveChild, 1);
            return 0;
        }
        break;

    case WM_SHOWWINDOW:
    case WM_SETVISIBLE:
        if (ci->hwndChildMaximized) ci->mdiFlags &= ~MDIF_NEEDUPDATE;
        else MDI_PostUpdate(client, ci, SB_BOTH+1);
        break;

    case WM_SIZE:
        /* This is the only place where we switch to/from maximized state */
        /* do not change */
        TRACE("current active %p, maximized %p\n", ci->hwndActiveChild, ci->hwndChildMaximized);

        if( ci->hwndChildMaximized == hwnd && wParam != SIZE_MAXIMIZED )
        {
            HWND frame;

            ci->hwndChildMaximized = 0;

            frame = GetParent(client);
            MDI_RestoreFrameMenu( frame, hwnd );
            MDI_UpdateFrameText( frame, client, TRUE, NULL );
        }

        if( wParam == SIZE_MAXIMIZED )
        {
            HWND frame, hMaxChild = ci->hwndChildMaximized;

            if( hMaxChild == hwnd ) break;

            if( hMaxChild)
            {
                SendMessageW( hMaxChild, WM_SETREDRAW, FALSE, 0 );

                MDI_RestoreFrameMenu( GetParent(client), hMaxChild );
                NtUserShowWindow( hMaxChild, SW_SHOWNOACTIVATE );

                SendMessageW( hMaxChild, WM_SETREDRAW, TRUE, 0 );
            }

            TRACE("maximizing child %p\n", hwnd );

            /* keep track of the maximized window. */
            ci->hwndChildMaximized = hwnd; /* !!! */

            frame = GetParent(client);
            MDI_AugmentFrameMenu( frame, hwnd );
            MDI_UpdateFrameText( frame, client, TRUE, NULL );
        }

        if( wParam == SIZE_MINIMIZED )
        {
            HWND switchTo = MDI_GetWindow( ci, hwnd, TRUE, WS_MINIMIZE );

            if (!switchTo) switchTo = hwnd;
            SendMessageW( switchTo, WM_CHILDACTIVATE, 0, 0 );
	}

        MDI_PostUpdate(client, ci, SB_BOTH+1);
        break;

    case WM_NEXTMENU:
        {
            MDINEXTMENU *next_menu = (MDINEXTMENU *)lParam;
            HWND parent = GetParent(client);

            if( wParam == VK_LEFT )  /* switch to frame system menu */
            {
                next_menu->hmenuNext = NtUserGetWindowSysSubMenu( parent );
            }
            if( wParam == VK_RIGHT )  /* to frame menu bar */
            {
                next_menu->hmenuNext = GetMenu(parent);
            }
            next_menu->hwndNext = parent;
            return 0;
        }

    case WM_SYSCHAR:
        if (wParam == '-')
        {
            SendMessageW( hwnd, WM_SYSCOMMAND, SC_KEYMENU, VK_SPACE);
            return 0;
        }
        break;

    case WM_DESTROY:
        /* Remove itself from the Window menu */
        MDI_RefreshMenu(ci);
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

/**********************************************************************
 *		CreateMDIWindowA (USER32.@) Creates a MDI child
 *
 * RETURNS
 *    Success: Handle to created window
 *    Failure: NULL
 */
HWND WINAPI CreateMDIWindowA(
    LPCSTR lpClassName,    /* [in] Pointer to registered child class name */
    LPCSTR lpWindowName,   /* [in] Pointer to window name */
    DWORD dwStyle,         /* [in] Window style */
    INT X,               /* [in] Horizontal position of window */
    INT Y,               /* [in] Vertical position of window */
    INT nWidth,          /* [in] Width of window */
    INT nHeight,         /* [in] Height of window */
    HWND hWndParent,     /* [in] Handle to parent window */
    HINSTANCE hInstance, /* [in] Handle to application instance */
    LPARAM lParam)         /* [in] Application-defined value */
{
    TRACE("(%s,%s,%08lx,%d,%d,%d,%d,%p,%p,%08Ix)\n",
          debugstr_a(lpClassName),debugstr_a(lpWindowName),dwStyle,X,Y,
          nWidth,nHeight,hWndParent,hInstance,lParam);

    return CreateWindowExA(WS_EX_MDICHILD, lpClassName, lpWindowName,
                           dwStyle, X, Y, nWidth, nHeight, hWndParent,
                           0, hInstance, (LPVOID)lParam);
}

/***********************************************************************
 *		CreateMDIWindowW (USER32.@) Creates a MDI child
 *
 * RETURNS
 *    Success: Handle to created window
 *    Failure: NULL
 */
HWND WINAPI CreateMDIWindowW(
    LPCWSTR lpClassName,    /* [in] Pointer to registered child class name */
    LPCWSTR lpWindowName,   /* [in] Pointer to window name */
    DWORD dwStyle,         /* [in] Window style */
    INT X,               /* [in] Horizontal position of window */
    INT Y,               /* [in] Vertical position of window */
    INT nWidth,          /* [in] Width of window */
    INT nHeight,         /* [in] Height of window */
    HWND hWndParent,     /* [in] Handle to parent window */
    HINSTANCE hInstance, /* [in] Handle to application instance */
    LPARAM lParam)         /* [in] Application-defined value */
{
    TRACE("(%s,%s,%08lx,%d,%d,%d,%d,%p,%p,%08Ix)\n",
          debugstr_w(lpClassName), debugstr_w(lpWindowName), dwStyle, X, Y,
          nWidth, nHeight, hWndParent, hInstance, lParam);

    return CreateWindowExW(WS_EX_MDICHILD, lpClassName, lpWindowName,
                           dwStyle, X, Y, nWidth, nHeight, hWndParent,
                           0, hInstance, (LPVOID)lParam);
}

/**********************************************************************
 *		TranslateMDISysAccel (USER32.@)
 */
BOOL WINAPI TranslateMDISysAccel( HWND hwndClient, LPMSG msg )
{
    if (msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN)
    {
        MDICLIENTINFO *ci = get_client_info( hwndClient );
        WPARAM wParam = 0;

        if (!ci || !IsWindowEnabled(ci->hwndActiveChild)) return FALSE;

        /* translate if the Ctrl key is down and Alt not. */

        if( (NtUserGetKeyState(VK_CONTROL) & 0x8000) && !(NtUserGetKeyState(VK_MENU) & 0x8000))
        {
            switch( msg->wParam )
            {
            case VK_F6:
            case VK_TAB:
                wParam = ( NtUserGetKeyState(VK_SHIFT) & 0x8000 ) ? SC_NEXTWINDOW : SC_PREVWINDOW;
                break;
            case VK_F4:
            case VK_RBUTTON:
                if (is_close_enabled(ci->hwndActiveChild, 0))
                {
                    wParam = SC_CLOSE;
                    break;
                }
                /* fall through */
            default:
                return FALSE;
            }
            TRACE("wParam = %04Ix\n", wParam);
            SendMessageW(ci->hwndActiveChild, WM_SYSCOMMAND, wParam, msg->wParam);
            return TRUE;
        }
    }
    return FALSE; /* failure */
}

/***********************************************************************
 *		CalcChildScroll (USER32.@)
 */
void WINAPI CalcChildScroll( HWND hwnd, INT scroll )
{
    DPI_AWARENESS_CONTEXT context;
    SCROLLINFO info;
    RECT childRect, clientRect;
    HWND *list;
    DWORD style;

    context = SetThreadDpiAwarenessContext( GetWindowDpiAwarenessContext( hwnd ));

    GetClientRect( hwnd, &clientRect );
    SetRectEmpty( &childRect );

    if ((list = WIN_ListChildren( hwnd )))
    {
        int i;
        for (i = 0; list[i]; i++)
        {
            style = GetWindowLongW( list[i], GWL_STYLE );
            if (style & WS_MAXIMIZE)
            {
                HeapFree( GetProcessHeap(), 0, list );
                NtUserShowScrollBar( hwnd, SB_BOTH, FALSE );
                goto done;
            }
            if (style & WS_VISIBLE)
            {
                RECT rect;
                NtUserGetChildRect( list[i], &rect );
                UnionRect( &childRect, &rect, &childRect );
            }
        }
        HeapFree( GetProcessHeap(), 0, list );
    }
    UnionRect( &childRect, &clientRect, &childRect );

    /* set common info values */
    info.cbSize = sizeof(info);
    info.fMask = SIF_POS | SIF_RANGE;

    /* set the specific values and apply but only if window style allows */
    style = GetWindowLongW( hwnd, GWL_STYLE );
    switch( scroll )
    {
	case SB_BOTH:
	case SB_HORZ:
                        if (style & (WS_HSCROLL | WS_VSCROLL))
                        {
                            info.nMin = childRect.left;
                            info.nMax = childRect.right - clientRect.right;
                            info.nPos = clientRect.left - childRect.left;
                            NtUserSetScrollInfo(hwnd, SB_HORZ, &info, TRUE);
                        }
			if (scroll == SB_HORZ) break;
			/* fall through */
	case SB_VERT:
                        if (style & (WS_HSCROLL | WS_VSCROLL))
                        {
                            info.nMin = childRect.top;
                            info.nMax = childRect.bottom - clientRect.bottom;
                            info.nPos = clientRect.top - childRect.top;
                            NtUserSetScrollInfo(hwnd, SB_VERT, &info, TRUE);
                        }
			break;
    }

done:
    SetThreadDpiAwarenessContext( context );
}


/***********************************************************************
 *		ScrollChildren (USER32.@)
 */
void WINAPI ScrollChildren(HWND hWnd, UINT uMsg, WPARAM wParam,
                             LPARAM lParam)
{
    DPI_AWARENESS_CONTEXT context;
    INT newPos = -1;
    INT curPos, length, minPos, maxPos, shift;
    RECT rect;

    context = SetThreadDpiAwarenessContext( GetWindowDpiAwarenessContext( hWnd ));
    GetClientRect( hWnd, &rect );

    switch(uMsg)
    {
    case WM_HSCROLL:
	GetScrollRange(hWnd,SB_HORZ,&minPos,&maxPos);
	curPos = GetScrollPos(hWnd,SB_HORZ);
	length = (rect.right - rect.left) / 2;
	shift = GetSystemMetrics(SM_CYHSCROLL);
        break;
    case WM_VSCROLL:
	GetScrollRange(hWnd,SB_VERT,&minPos,&maxPos);
	curPos = GetScrollPos(hWnd,SB_VERT);
	length = (rect.bottom - rect.top) / 2;
	shift = GetSystemMetrics(SM_CXVSCROLL);
        break;
    default:
        goto done;
    }

    switch( wParam )
    {
	case SB_LINEUP:
		        newPos = curPos - shift;
			break;
	case SB_LINEDOWN:
			newPos = curPos + shift;
			break;
	case SB_PAGEUP:
			newPos = curPos - length;
			break;
	case SB_PAGEDOWN:
			newPos = curPos + length;
			break;

	case SB_THUMBPOSITION:
			newPos = LOWORD(lParam);
			break;

	case SB_THUMBTRACK:
			goto done;

	case SB_TOP:
			newPos = minPos;
			break;
	case SB_BOTTOM:
			newPos = maxPos;
			break;
	case SB_ENDSCROLL:
			CalcChildScroll(hWnd,(uMsg == WM_VSCROLL)?SB_VERT:SB_HORZ);
			goto done;
    }

    if( newPos > maxPos )
	newPos = maxPos;
    else
	if( newPos < minPos )
	    newPos = minPos;

    SetScrollPos(hWnd, (uMsg == WM_VSCROLL)?SB_VERT:SB_HORZ , newPos, TRUE);

    if( uMsg == WM_VSCROLL )
	NtUserScrollWindowEx( hWnd ,0 ,curPos - newPos, NULL, NULL, 0, NULL,
                              SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN );
    else
	NtUserScrollWindowEx( hWnd ,curPos - newPos, 0, NULL, NULL, 0, NULL,
                              SW_INVALIDATE | SW_ERASE | SW_SCROLLCHILDREN );
done:
    SetThreadDpiAwarenessContext( context );
}


/******************************************************************************
 *		CascadeWindows (USER32.@) Cascades MDI child windows
 *
 * RETURNS
 *    Success: Number of cascaded windows.
 *    Failure: 0
 */

typedef struct CASCADE_INFO
{
    HWND top;
    UINT flags;
    HWND parent;
    HWND desktop;
    HWND tray_wnd;
    HWND progman;
    HWND *wnd_array;
    DWORD wnd_count;
} CASCADE_INFO;

static BOOL CALLBACK GetCascadeChildProc(HWND hwnd, LPARAM lParam)
{
    DWORD count, size;
    HWND *wnd_array;
    CASCADE_INFO *info = (CASCADE_INFO *)lParam;

    if (hwnd == info->desktop || hwnd == info->tray_wnd ||
        hwnd == info->progman || hwnd == info->top)
    {
        return TRUE;
    }

    if (info->parent && GetParent(hwnd) != info->parent)
        return TRUE;

    if ((info->flags & MDITILE_SKIPDISABLED) && !IsWindowEnabled(hwnd))
        return TRUE;

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
        return TRUE;

    count = info->wnd_count;
    size = (count + 1) * sizeof(HWND);

    if (count == 0 || !info->wnd_array)
    {
        count = 0;
        info->wnd_array = (HWND *)heap_alloc(size);
    }
    else
    {
        wnd_array = (HWND *)heap_realloc(info->wnd_array, size);
        if (!wnd_array)
        {
            heap_free(info->wnd_array);
        }
        info->wnd_array = wnd_array;
    }

    if (!info->wnd_array)
    {
        info->wnd_count = 0;
        return FALSE;
    }

    info->wnd_array[count] = hwnd;
    info->wnd_count = count + 1;
    return TRUE;
}

static BOOL
QuerySizeFix(HWND hwnd, INT *pcx, INT *pcy)
{
    MINMAXINFO mmi;
    DWORD_PTR result;

    mmi.ptMinTrackSize.x = mmi.ptMinTrackSize.y = 0;
    mmi.ptMaxTrackSize.x = mmi.ptMaxTrackSize.y = MAXLONG;
    if (SendMessageTimeoutW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mmi,
                            SMTO_ABORTIFHUNG | SMTO_NORMAL, 120, &result))
    {
        *pcx = min(max(*pcx, mmi.ptMinTrackSize.x), mmi.ptMaxTrackSize.x);
        *pcy = min(max(*pcy, mmi.ptMinTrackSize.y), mmi.ptMaxTrackSize.y);
        return TRUE;
    }
    return FALSE;
}

WORD WINAPI
CascadeWindows (HWND hwndParent, UINT wFlags, const RECT *lpRect,
		UINT cKids, const HWND *lpKids)
{
    CASCADE_INFO info;
    HWND hwnd, top, prev;
    HMONITOR monitor;
    MONITORINFO mi;
    RECT work_rect, wnd_rect;
    DWORD i, ret = 0;
    INT x, y, width, height, new_width, new_height, work_width, work_height, dx, dy;
    HDWP hDWP;
    POINT pt;

    TRACE("(%p,0x%08x,%p,%u,%p)\n", hwndParent, wFlags, lpRect, cKids, lpKids);

    top = GetTopWindow(hwndParent);

    ZeroMemory(&info, sizeof(info));
    info.desktop = GetDesktopWindow();
    info.tray_wnd = FindWindowW(shelltray, NULL);
    info.progman = FindWindowW(progman, NULL);
    info.parent = hwndParent;
    info.flags = wFlags;

    if (cKids == 0 || lpKids == NULL)
    {
        info.top = top;
        EnumChildWindows(hwndParent, GetCascadeChildProc, (LPARAM)&info);

        info.top = NULL;
        GetCascadeChildProc(top, (LPARAM)&info);
    }
    else
    {
        info.wnd_count = cKids;
        info.wnd_array = (HWND *)lpKids;
    }

    if (info.wnd_count == 0 || info.wnd_array == NULL)
        return ret;

    if (lpRect)
    {
        work_rect = *lpRect;
    }
    else if (hwndParent)
    {
        GetClientRect(hwndParent, &work_rect);
    }
    else
    {
        pt.x = pt.y = 0;
        monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitor, &mi);
        work_rect = mi.rcWork;
    }

    hDWP = NtUserBeginDeferWindowPos( info.wnd_count );
    if (hDWP == NULL)
        goto cleanup;

    x = work_rect.left;
    y = work_rect.top;
    dx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXSIZE);
    dy = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CYSIZE);
    work_width = work_rect.right - work_rect.left;
    work_height = work_rect.bottom - work_rect.top;
    prev = NULL;
    for (i = info.wnd_count; i > 0;)    /* in reverse order */
    {
        --i;
        hwnd = info.wnd_array[i];

        if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
            continue;

        if ((info.flags & MDITILE_SKIPDISABLED) && !IsWindowEnabled(hwnd))
            continue;

        if (IsZoomed(hwnd))
            NtUserShowWindow(hwnd, SW_RESTORE | SW_SHOWNA);

        GetWindowRect(hwnd, &wnd_rect);
        new_width = width = wnd_rect.right - wnd_rect.left;
        new_height = height = wnd_rect.bottom - wnd_rect.top;

        /* if we can change the window size and it is not only one */
        if (info.wnd_count != 1 && (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME))
        {
            /* check the size */
#define THRESHOLD(xy) (((xy) * 5) / 7)      /* in the rate 5/7 */
            new_width = min(new_width, THRESHOLD(work_width));
            new_height = min(new_height, THRESHOLD(work_height));
#undef THRESHOLD
            if (width != new_width || height != new_height)
            {
                /* too large. shrink if we can */
                if (QuerySizeFix(hwnd, &new_width, &new_height))
                {
                    width = new_width;
                    height = new_height;
                }
            }
        }

        if (x + width > work_rect.right)
            x = work_rect.left;
        if (y + height > work_rect.bottom)
            y = work_rect.top;

        hDWP = DeferWindowPos(hDWP, hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE);
        if (hDWP == NULL)
        {
            ret = 0;
            goto cleanup;
        }

        x += dx;
        y += dy;
        prev = hwnd;
        ++ret;
    }

    EndDeferWindowPos(hDWP);

    if (prev)
        NtUserSetForegroundWindow( prev );

cleanup:
    heap_free(info.wnd_array);

    return (WORD)ret;
}


/***********************************************************************
 *		CascadeChildWindows (USER32.@)
 */
WORD WINAPI CascadeChildWindows( HWND parent, UINT flags )
{
    return CascadeWindows( parent, flags, NULL, 0, NULL );
}


/******************************************************************************
 *		TileWindows (USER32.@) Tiles MDI child windows
 *
 * RETURNS
 *    Success: Number of tiled windows.
 *    Failure: 0
 */
WORD WINAPI
TileWindows (HWND hwndParent, UINT wFlags, const RECT *lpRect,
	     UINT cKids, const HWND *lpKids)
{
    HWND hwnd, hwndTop, hwndPrev;
    CASCADE_INFO info;
    RECT rcWork, rcWnd;
    DWORD i, iRow, iColumn, cRows, cColumns, ret = 0;
    INT x, y, cx, cy, cxNew, cyNew, cxWork, cyWork, cxCell, cyCell, cxMin2, cyMin3;
    HDWP hDWP;
    MONITORINFO mi;
    HMONITOR hMon;
    POINT pt;

    TRACE("(%p,0x%08x,...,%u,...)\n", hwndParent, wFlags, cKids);

    hwndTop = GetTopWindow(hwndParent);

    ZeroMemory(&info, sizeof(info));
    info.desktop = GetDesktopWindow();
    info.tray_wnd = FindWindowW(shelltray, NULL);
    info.progman = FindWindowW(progman, NULL);
    info.parent = hwndParent;
    info.flags = wFlags;

    if (cKids == 0 || lpKids == NULL)
    {
        info.top = hwndTop;
        EnumChildWindows(hwndParent, GetCascadeChildProc, (LPARAM)&info);

        info.top = NULL;
        GetCascadeChildProc(hwndTop, (LPARAM)&info);
    }
    else
    {
        info.wnd_count = cKids;
        info.wnd_array = (HWND *)lpKids;
    }

    if (info.wnd_count == 0 || info.wnd_array == NULL)
        return ret;

    if (lpRect)
    {
        rcWork = *lpRect;
    }
    else if (hwndParent)
    {
        GetClientRect(hwndParent, &rcWork);
    }
    else
    {
        pt.x = pt.y = 0;
        hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(hMon, &mi);
        rcWork = mi.rcWork;
    }

    cxWork = rcWork.right - rcWork.left;
    cyWork = rcWork.bottom - rcWork.top;

    cxMin2 = GetSystemMetrics(SM_CXMIN) * 2;
    cyMin3 = GetSystemMetrics(SM_CYMIN) * 3;

    /* calculate the numbers and widths of columns and rows */
    if (info.flags & MDITILE_HORIZONTAL)
    {
        cColumns = info.wnd_count;
        cRows = 1;
        for (;;)
        {
            cxCell = cxWork / cColumns;
            cyCell = cyWork / cRows;
            if (cyCell <= cyMin3 || cxCell >= cxMin2)
                break;

            ++cRows;
            cColumns = (info.wnd_count + cRows - 1) / cRows;
        }
    }
    else
    {
        cRows = info.wnd_count;
        cColumns = 1;
        for (;;)
        {
            cxCell = cxWork / cColumns;
            cyCell = cyWork / cRows;
            if (cxCell <= cxMin2 || cyCell >= cyMin3)
                break;

            ++cColumns;
            cRows = (info.wnd_count + cColumns - 1) / cColumns;
        }
    }

    hDWP = NtUserBeginDeferWindowPos( info.wnd_count );
    if (hDWP == NULL)
        goto cleanup;

    x = rcWork.left;
    y = rcWork.top;
    hwndPrev = NULL;
    iRow = iColumn = 0;
    for (i = info.wnd_count; i > 0;)    /* in reverse order */
    {
        --i;
        hwnd = info.wnd_array[i];

        if (IsZoomed(hwnd))
            NtUserShowWindow(hwnd, SW_RESTORE | SW_SHOWNA);

        GetWindowRect(hwnd, &rcWnd);
        cx = rcWnd.right - rcWnd.left;
        cy = rcWnd.bottom - rcWnd.top;

        /* if we can change the window size */
        if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_THICKFRAME)
        {
            cxNew = cxCell;
            cyNew = cyCell;
            /* shrink if we can */
            if (QuerySizeFix(hwnd, &cxNew, &cyNew))
            {
                cx = cxNew;
                cy = cyNew;
            }
        }

        hDWP = DeferWindowPos(hDWP, hwnd, HWND_TOP, x, y, cx, cy, SWP_NOACTIVATE);
        if (hDWP == NULL)
        {
            ret = 0;
            goto cleanup;
        }

        if (info.flags & MDITILE_HORIZONTAL)
        {
            x += cxCell;
            ++iColumn;
            if (iColumn >= cColumns)
            {
                iColumn = 0;
                ++iRow;
                x = rcWork.left;
                y += cyCell;
            }
        }
        else
        {
            y += cyCell;
            ++iRow;
            if (iRow >= cRows)
            {
                iRow = 0;
                ++iColumn;
                x += cxCell;
                y = rcWork.top;
            }
        }
        hwndPrev = hwnd;
        ++ret;
    }

    EndDeferWindowPos(hDWP);

    if (hwndPrev)
        NtUserSetForegroundWindow( hwndPrev );

cleanup:
    if (cKids == 0 || lpKids == NULL)
        HeapFree(GetProcessHeap(), 0, info.wnd_array);

    return (WORD)ret;
}


/***********************************************************************
 *		TileChildWindows (USER32.@)
 */
WORD WINAPI TileChildWindows( HWND parent, UINT flags )
{
    return TileWindows( parent, flags, NULL, 0, NULL );
}


/************************************************************************
 *              "More Windows..." functionality
 */

/*              MDI_MoreWindowsDlgProc
 *
 *    This function will process the messages sent to the "More Windows..."
 *    dialog.
 *    Return values:  0    = cancel pressed
 *                    HWND = ok pressed or double-click in the list...
 *
 */

static INT_PTR WINAPI MDI_MoreWindowsDlgProc (HWND hDlg, UINT iMsg, WPARAM wParam, LPARAM lParam)
{
    switch (iMsg)
    {
       case WM_INITDIALOG:
       {
           UINT widest       = 0;
           UINT length;
           UINT i;
           MDICLIENTINFO *ci = get_client_info( (HWND)lParam );
           HWND hListBox = GetDlgItem(hDlg, MDI_IDC_LISTBOX);

           for (i = 0; i < ci->nActiveChildren; i++)
           {
               WCHAR buffer[MDI_MAXTITLELENGTH];

               if (!NtUserInternalGetWindowText(ci->child[i], buffer, ARRAY_SIZE(buffer)))
                   continue;
               SendMessageW(hListBox, LB_ADDSTRING, 0, (LPARAM)buffer );
               SendMessageW(hListBox, LB_SETITEMDATA, i, (LPARAM)ci->child[i] );
               length = lstrlenW(buffer);  /* FIXME: should use GetTextExtentPoint */
               if (length > widest)
                   widest = length;
           }
           /* Make sure the horizontal scrollbar scrolls ok */
           SendMessageW(hListBox, LB_SETHORIZONTALEXTENT, widest * 6, 0);

           /* Set the current selection */
           SendMessageW(hListBox, LB_SETCURSEL, MDI_MOREWINDOWSLIMIT, 0);
           return TRUE;
       }

       case WM_COMMAND:
           switch (LOWORD(wParam))
           {
                default:
                    if (HIWORD(wParam) != LBN_DBLCLK) break;
                    /* fall through */
                case IDOK:
                {
                    /*  windows are sorted by menu ID, so we must return the
                     *  window associated to the given id
                     */
                    HWND hListBox     = GetDlgItem(hDlg, MDI_IDC_LISTBOX);
                    UINT index        = SendMessageW(hListBox, LB_GETCURSEL, 0, 0);
                    LRESULT res = SendMessageW(hListBox, LB_GETITEMDATA, index, 0);
                    EndDialog(hDlg, res);
                    return TRUE;
                }
                case IDCANCEL:
                    EndDialog(hDlg, 0);
                    return TRUE;
           }
           break;
    }
    return FALSE;
}

/*
 *
 *                      MDI_MoreWindowsDialog
 *
 *     Prompts the user with a listbox containing the opened
 *     documents. The user can then choose a windows and click
 *     on OK to set the current window to the one selected, or
 *     CANCEL to cancel. The function returns a handle to the
 *     selected window.
 */

static HWND MDI_MoreWindowsDialog(HWND hwnd)
{
    LPCVOID template;
    HRSRC hRes;
    HANDLE hDlgTmpl;

    hRes = FindResourceA(user32_module, "MDI_MOREWINDOWS", (LPSTR)RT_DIALOG);

    if (hRes == 0)
        return 0;

    hDlgTmpl = LoadResource(user32_module, hRes );

    if (hDlgTmpl == 0)
        return 0;

    template = LockResource( hDlgTmpl );

    if (template == 0)
        return 0;

    return (HWND) DialogBoxIndirectParamA(user32_module, template, hwnd,
                                          MDI_MoreWindowsDlgProc, (LPARAM) hwnd);
}
