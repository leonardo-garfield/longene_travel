/*
 * Trash virtual folder support. The trashing engine is implemented in trash.c
 *
 * Copyright (C) 2006 Mikolaj Zalewski
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
 */

#include "config.h"

#define COBJMACROS
#define NONAMELESSUNION

#include <stdarg.h>

#include "winerror.h"
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "winuser.h"
#include "ntquery.h"
#include "shlwapi.h"
#include "shlobj.h"
#include "shresdef.h"
#include "wine/debug.h"

#include "shell32_main.h"
#include "enumidlist.h"
#include "xdg.h"

WINE_DEFAULT_DEBUG_CHANNEL(recyclebin);

typedef struct
{
    int column_name_id;
    const GUID *fmtId;
    DWORD pid;
    int pcsFlags;
    int fmt;
    int cxChars;
} columninfo;

static const columninfo RecycleBinColumns[] =
{
    {IDS_SHV_COLUMN1,        &FMTID_Storage,   PID_STG_NAME,       SHCOLSTATE_TYPE_STR|SHCOLSTATE_ONBYDEFAULT,  LVCFMT_LEFT,  30},
    {IDS_SHV_COLUMN_DELFROM, &FMTID_Displaced, PID_DISPLACED_FROM, SHCOLSTATE_TYPE_STR|SHCOLSTATE_ONBYDEFAULT,  LVCFMT_LEFT,  30},
    {IDS_SHV_COLUMN_DELDATE, &FMTID_Displaced, PID_DISPLACED_DATE, SHCOLSTATE_TYPE_DATE|SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT,  20},
    {IDS_SHV_COLUMN2,        &FMTID_Storage,   PID_STG_SIZE,       SHCOLSTATE_TYPE_INT|SHCOLSTATE_ONBYDEFAULT,  LVCFMT_RIGHT, 20},
    {IDS_SHV_COLUMN3,        &FMTID_Storage,   PID_STG_STORAGETYPE,SHCOLSTATE_TYPE_INT|SHCOLSTATE_ONBYDEFAULT,  LVCFMT_LEFT,  20},
    {IDS_SHV_COLUMN4,        &FMTID_Storage,   PID_STG_WRITETIME,  SHCOLSTATE_TYPE_DATE|SHCOLSTATE_ONBYDEFAULT, LVCFMT_LEFT,  20},
/*    {"creation time",  &FMTID_Storage,   PID_STG_CREATETIME, SHCOLSTATE_TYPE_DATE,                        LVCFMT_LEFT,  20}, */
/*    {"attribs",        &FMTID_Storage,   PID_STG_ATTRIBUTES, SHCOLSTATE_TYPE_STR,                         LVCFMT_LEFT,  20},       */
};

#define COLUMN_NAME    0
#define COLUMN_DELFROM 1
#define COLUMN_DATEDEL 2
#define COLUMN_SIZE    3
#define COLUMN_TYPE    4
#define COLUMN_MTIME   5

#define COLUMNS_COUNT  6

static HRESULT FormatDateTime(LPWSTR buffer, int size, FILETIME ft)
{
    FILETIME lft;
    SYSTEMTIME time;
    int ret;

    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &time);

    ret = GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &time, NULL, buffer, size);
    if (ret>0 && ret<size)
    {
        /* Append space + time without seconds */
        buffer[ret-1] = ' ';
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &time, NULL, &buffer[ret], size - ret);
    }

    return (ret!=0 ? E_FAIL : S_OK);
}

/*
 * Recycle Bin folder
 */

typedef struct tagRecycleBin
{
    const IShellFolder2Vtbl *lpVtbl;
    const IPersistFolder2Vtbl *lpPersistFolderVtbl;
    LONG refCount;

    LPITEMIDLIST pidl;
} RecycleBin;

static const IShellFolder2Vtbl recycleBinVtbl;
static const IPersistFolder2Vtbl recycleBinPersistVtbl;

static RecycleBin *impl_from_IPersistFolder(IPersistFolder2 *iface)
{
    return (RecycleBin *)((char *)iface - FIELD_OFFSET(RecycleBin, lpPersistFolderVtbl));
}

static void RecycleBin_Destructor(RecycleBin *This);

HRESULT WINAPI RecycleBin_Constructor(IUnknown *pUnkOuter, REFIID riid, LPVOID *ppOutput)
{
    RecycleBin *obj;
    HRESULT ret;
    if (pUnkOuter)
        return CLASS_E_NOAGGREGATION;

    obj = SHAlloc(sizeof(RecycleBin));
    if (obj == NULL)
        return E_OUTOFMEMORY;
    ZeroMemory(obj, sizeof(RecycleBin));
    obj->lpVtbl = &recycleBinVtbl;
    obj->lpPersistFolderVtbl = &recycleBinPersistVtbl;
    if (FAILED(ret = IUnknown_QueryInterface((IUnknown *)obj, riid, ppOutput)))
    {
        RecycleBin_Destructor(obj);
        return ret;
    }
/*    InterlockedIncrement(&objCount);*/
    return S_OK;
}

static void RecycleBin_Destructor(RecycleBin *This)
{
/*    InterlockedDecrement(&objCount);*/
    SHFree(This->pidl);
    SHFree(This);
}

static HRESULT WINAPI RecycleBin_QueryInterface(IShellFolder2 *iface, REFIID riid, void **ppvObject)
{
    RecycleBin *This = (RecycleBin *)iface;
    TRACE("(%p, %s, %p)\n", This, debugstr_guid(riid), ppvObject);

    *ppvObject = NULL;
    if (IsEqualGUID(riid, &IID_IUnknown) || IsEqualGUID(riid, &IID_IShellFolder)
            || IsEqualGUID(riid, &IID_IShellFolder2))
        *ppvObject = This;

    if (IsEqualGUID(riid, &IID_IPersist) || IsEqualGUID(riid, &IID_IPersistFolder)
            || IsEqualGUID(riid, &IID_IPersistFolder2))
        *ppvObject = &This->lpPersistFolderVtbl;

    if (*ppvObject != NULL)
    {
        IUnknown_AddRef((IUnknown *)*ppvObject);
        return S_OK;
    }
    WARN("no interface %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI RecycleBin_AddRef(IShellFolder2 *iface)
{
    RecycleBin *This = (RecycleBin *)iface;
    TRACE("(%p)\n", This);
    return InterlockedIncrement(&This->refCount);
}

static ULONG WINAPI RecycleBin_Release(IShellFolder2 *iface)
{
    RecycleBin *This = (RecycleBin *)iface;
    LONG result;

    TRACE("(%p)\n", This);
    result = InterlockedDecrement(&This->refCount);
    if (result == 0)
    {
        TRACE("Destroy object\n");
        RecycleBin_Destructor(This);
    }
    return result;
}

static HRESULT WINAPI RecycleBin_ParseDisplayName(IShellFolder2 *This, HWND hwnd, LPBC pbc,
            LPOLESTR pszDisplayName, ULONG *pchEaten, LPITEMIDLIST *ppidl,
            ULONG *pdwAttributes)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_EnumObjects(IShellFolder2 *iface, HWND hwnd, SHCONTF grfFlags, IEnumIDList **ppenumIDList)
{
    RecycleBin *This = (RecycleBin *)iface;
    IEnumIDList *list;
    LPITEMIDLIST *pidls;
    HRESULT ret;
    int pidls_count;
    int i=0;

    TRACE("(%p, %p, %x, %p)\n", This, hwnd, grfFlags, ppenumIDList);

    if (grfFlags & SHCONTF_NONFOLDERS)
    {
        *ppenumIDList = NULL;
        if (FAILED(ret = TRASH_EnumItems(&pidls, &pidls_count)))
            return ret;

        list = IEnumIDList_Constructor();
        if (list == NULL)
            goto failed;
        for (i=0; i<pidls_count; i++)
            if (!AddToEnumList(list, pidls[i]))
                goto failed;
        *ppenumIDList = list;
    }
    else
    {
        *ppenumIDList = IEnumIDList_Constructor();
        if (*ppenumIDList == NULL)
            return E_OUTOFMEMORY;
    }
    
    return S_OK;

failed:
    if (list)
        IEnumIDList_Release(list);
    for (; i<pidls_count; i++)
        ILFree(pidls[i]);
    SHFree(pidls);
    return E_OUTOFMEMORY;
}

static HRESULT WINAPI RecycleBin_BindToObject(IShellFolder2 *This, LPCITEMIDLIST pidl, LPBC pbc, REFIID riid, void **ppv)
{
    FIXME("(%p, %p, %p, %s, %p) - stub\n", This, pidl, pbc, debugstr_guid(riid), ppv);
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_BindToStorage(IShellFolder2 *This, LPCITEMIDLIST pidl, LPBC pbc, REFIID riid, void **ppv)
{
    FIXME("(%p, %p, %p, %s, %p) - stub\n", This, pidl, pbc, debugstr_guid(riid), ppv);
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_CompareIDs(IShellFolder2 *iface, LPARAM lParam, LPCITEMIDLIST pidl1, LPCITEMIDLIST pidl2)
{
    RecycleBin *This = (RecycleBin *)iface;

    /* TODO */
    TRACE("(%p, %p, %p, %p)\n", This, (void *)lParam, pidl1, pidl2);
    if (pidl1->mkid.cb != pidl2->mkid.cb)
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, pidl1->mkid.cb - pidl2->mkid.cb);
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, (unsigned short)memcmp(pidl1->mkid.abID, pidl2->mkid.abID, pidl1->mkid.cb));
}

static HRESULT WINAPI RecycleBin_CreateViewObject(IShellFolder2 *iface, HWND hwndOwner, REFIID riid, void **ppv)
{
    RecycleBin *This = (RecycleBin *)iface;
    HRESULT ret;
    TRACE("(%p, %p, %s, %p)\n", This, hwndOwner, debugstr_guid(riid), ppv);

    *ppv = NULL;
    if (IsEqualGUID(riid, &IID_IShellView))
    {
        IShellView *tmp;
        CSFV sfv;

        ZeroMemory(&sfv, sizeof(sfv));
        sfv.cbSize = sizeof(sfv);
        sfv.pshf = (IShellFolder *)This;

        TRACE("Calling SHCreateShellFolderViewEx\n");
        ret = SHCreateShellFolderViewEx(&sfv, &tmp);
        TRACE("Result: %08x, output: %p\n", (unsigned int)ret, tmp);
        *ppv = tmp;
        return ret;
    }

    return E_NOINTERFACE;
}

static HRESULT WINAPI RecycleBin_GetAttributesOf(IShellFolder2 *This, UINT cidl, LPCITEMIDLIST *apidl,
                                   SFGAOF *rgfInOut)
{
    TRACE("(%p, %d, {%p, ...}, {%x})\n", This, cidl, apidl[0], *rgfInOut);
    *rgfInOut &= SFGAO_CANMOVE|SFGAO_CANDELETE|SFGAO_HASPROPSHEET|SFGAO_FILESYSTEM;
    return S_OK;
}

static HRESULT WINAPI RecycleBin_GetUIObjectOf(IShellFolder2 *This, HWND hwndOwner, UINT cidl, LPCITEMIDLIST *apidl,
                      REFIID riid, UINT *rgfReserved, void **ppv)
{
    FIXME("(%p, %p, %d, {%p, ...}, %s, %p, %p): stub!\n", This, hwndOwner, cidl, apidl[0], debugstr_guid(riid), rgfReserved, ppv);
    *ppv = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_GetDisplayNameOf(IShellFolder2 *This, LPCITEMIDLIST pidl, SHGDNF uFlags, STRRET *pName)
{
    WIN32_FIND_DATAW data;

    TRACE("(%p, %p, %x, %p)\n", This, pidl, uFlags, pName);
    TRASH_UnpackItemID(&pidl->mkid, NULL, &data);
    pName->uType = STRRET_WSTR;
    pName->u.pOleStr = StrDupW(PathFindFileNameW(data.cFileName));
    if (pName->u.pOleStr == NULL)
        return E_OUTOFMEMORY;

    return S_OK;
}

static HRESULT WINAPI RecycleBin_SetNameOf(IShellFolder2 *This, HWND hwnd, LPCITEMIDLIST pidl, LPCOLESTR pszName,
            SHGDNF uFlags, LPITEMIDLIST *ppidlOut)
{
    TRACE("\n");
    return E_FAIL; /* not supported */
}

static HRESULT WINAPI RecycleBin_GetClassID(IPersistFolder2 *This, CLSID *pClassID)
{
    TRACE("(%p, %p)\n", This, pClassID);
    if (This == NULL || pClassID == NULL)
        return E_INVALIDARG;
    *pClassID = CLSID_RecycleBin;
    return S_OK;
}

static HRESULT WINAPI RecycleBin_Initialize(IPersistFolder2 *iface, LPCITEMIDLIST pidl)
{
    RecycleBin *This = impl_from_IPersistFolder(iface);
    TRACE("(%p, %p)\n", This, pidl);
    
    This->pidl = ILClone(pidl);
    if (This->pidl == NULL)
        return E_OUTOFMEMORY;
    return S_OK;
}

static HRESULT WINAPI RecycleBin_GetCurFolder(IPersistFolder2 *iface, LPITEMIDLIST *ppidl)
{
    RecycleBin *This = impl_from_IPersistFolder(iface);
    TRACE("\n");
    *ppidl = ILClone(This->pidl);
    return S_OK;
}

static HRESULT WINAPI RecycleBin_GetDefaultSearchGUID(IShellFolder2 *iface, GUID *pguid)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_EnumSearches(IShellFolder2 *iface, IEnumExtraSearch **ppEnum)
{
    FIXME("stub\n");
    *ppEnum = NULL;
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_GetDefaultColumn(IShellFolder2 *iface, DWORD dwReserved, ULONG *pSort, ULONG *pDisplay)
{
    RecycleBin *This = (RecycleBin *)iface;
    TRACE("(%p, %x, %p, %p)\n", This, dwReserved, pSort, pDisplay);
    *pSort = 0;
    *pDisplay = 0;
    return S_OK;
}

static HRESULT WINAPI RecycleBin_GetDefaultColumnState(IShellFolder2 *iface, UINT iColumn, SHCOLSTATEF *pcsFlags)
{
    RecycleBin *This = (RecycleBin *)iface;
    TRACE("(%p, %d, %p)\n", This, iColumn, pcsFlags);
    if (iColumn >= COLUMNS_COUNT)
        return E_INVALIDARG;
    *pcsFlags = RecycleBinColumns[iColumn].pcsFlags;
    return S_OK;
}

static HRESULT WINAPI RecycleBin_GetDetailsEx(IShellFolder2 *iface, LPCITEMIDLIST pidl, const SHCOLUMNID *pscid, VARIANT *pv)
{
    FIXME("stub\n");
    return E_NOTIMPL;
}

static HRESULT WINAPI RecycleBin_GetDetailsOf(IShellFolder2 *iface, LPCITEMIDLIST pidl, UINT iColumn, LPSHELLDETAILS pDetails)
{
    RecycleBin *This = (RecycleBin *)iface;
    WIN32_FIND_DATAW data;
    WCHAR buffer[MAX_PATH];

    TRACE("(%p, %p, %d, %p)\n", This, pidl, iColumn, pDetails);
    if (iColumn >= COLUMNS_COUNT)
        return E_FAIL;
    pDetails->fmt = RecycleBinColumns[iColumn].fmt;
    pDetails->cxChar = RecycleBinColumns[iColumn].cxChars;
    if (pidl == NULL)
    {
        pDetails->str.uType = STRRET_WSTR;
        LoadStringW(shell32_hInstance, RecycleBinColumns[iColumn].column_name_id, buffer, MAX_PATH);
        return SHStrDupW(buffer, &pDetails->str.u.pOleStr);
    }

    if (iColumn == COLUMN_NAME)
        return RecycleBin_GetDisplayNameOf(iface, pidl, SHGDN_NORMAL, &pDetails->str);

    TRASH_UnpackItemID(&pidl->mkid, NULL, &data);
    switch (iColumn)
    {
        case COLUMN_DATEDEL:
            FormatDateTime(buffer, MAX_PATH, data.ftLastAccessTime);
            break;
        case COLUMN_DELFROM:
            lstrcpyW(buffer, data.cFileName);
            PathRemoveFileSpecW(buffer);
            break;
        case COLUMN_SIZE:
            StrFormatKBSizeW(((LONGLONG)data.nFileSizeHigh<<32)|data.nFileSizeLow, buffer, MAX_PATH);
            break;
        case COLUMN_MTIME:
            FormatDateTime(buffer, MAX_PATH, data.ftLastWriteTime);
            break;
        case COLUMN_TYPE:
            /* TODO */
            buffer[0] = 0;
            break;
        default:
            return E_FAIL;
    }
    
    pDetails->str.uType = STRRET_WSTR;
    return SHStrDupW(buffer, &pDetails->str.u.pOleStr);
}

static HRESULT WINAPI RecycleBin_MapColumnToSCID(IShellFolder2 *iface, UINT iColumn, SHCOLUMNID *pscid)
{
    RecycleBin *This = (RecycleBin *)iface;
    TRACE("(%p, %d, %p)\n", This, iColumn, pscid);
    if (iColumn>=COLUMNS_COUNT)
        return E_INVALIDARG;
    pscid->fmtid = *RecycleBinColumns[iColumn].fmtId;
    pscid->pid = RecycleBinColumns[iColumn].pid;
    return S_OK;
}

static const IShellFolder2Vtbl recycleBinVtbl = 
{
    /* IUnknown */
    RecycleBin_QueryInterface,
    RecycleBin_AddRef,
    RecycleBin_Release,

    /* IShellFolder */
    RecycleBin_ParseDisplayName,
    RecycleBin_EnumObjects,
    RecycleBin_BindToObject,
    RecycleBin_BindToStorage,
    RecycleBin_CompareIDs,
    RecycleBin_CreateViewObject,
    RecycleBin_GetAttributesOf,
    RecycleBin_GetUIObjectOf,
    RecycleBin_GetDisplayNameOf,
    RecycleBin_SetNameOf,

    /* IShellFolder2 */
    RecycleBin_GetDefaultSearchGUID,
    RecycleBin_EnumSearches,
    RecycleBin_GetDefaultColumn,
    RecycleBin_GetDefaultColumnState,
    RecycleBin_GetDetailsEx,
    RecycleBin_GetDetailsOf,
    RecycleBin_MapColumnToSCID
};

static HRESULT WINAPI RecycleBin_IPersistFolder2_QueryInterface(IPersistFolder2 *This, REFIID riid, void **ppvObject)
{
    return RecycleBin_QueryInterface((IShellFolder2 *)impl_from_IPersistFolder(This), riid, ppvObject);
}

static ULONG WINAPI RecycleBin_IPersistFolder2_AddRef(IPersistFolder2 *This)
{
    return RecycleBin_AddRef((IShellFolder2 *)impl_from_IPersistFolder(This));
}

static ULONG WINAPI RecycleBin_IPersistFolder2_Release(IPersistFolder2 *This)
{
    return RecycleBin_Release((IShellFolder2 *)impl_from_IPersistFolder(This));
}

static const IPersistFolder2Vtbl recycleBinPersistVtbl =
{
    /* IUnknown */
    RecycleBin_IPersistFolder2_QueryInterface,
    RecycleBin_IPersistFolder2_AddRef,
    RecycleBin_IPersistFolder2_Release,

    /* IPersist */
    RecycleBin_GetClassID,
    /* IPersistFolder */
    RecycleBin_Initialize,
    /* IPersistFolder2 */
    RecycleBin_GetCurFolder
};

/*************************************************************************
 * SHUpdateRecycleBinIcon                                [SHELL32.@]
 *
 * Undocumented
 */
HRESULT WINAPI SHUpdateRecycleBinIcon(void)
{
    FIXME("stub\n");
    return S_OK;
}
