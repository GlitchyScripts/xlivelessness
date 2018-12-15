#include <winsock2.h>
#include "xdefs.h"
#include "../xlln/DebugText.h"
#include "xlive.h"
#include "xrender.h"
#include "xnet.h"
#include "../xlln/xlln.h"
#include "xsocket.h"
#include <time.h>
#include <d3d9.h>
#include <string>
#include <vector>
// Link with iphlpapi.lib
#include <iphlpapi.h>

BOOL xlive_users_info_changed[XLIVE_LOCAL_USER_COUNT];
XUSER_SIGNIN_INFO* xlive_users_info[XLIVE_LOCAL_USER_COUNT];

struct NOTIFY_LISTENER {
	HANDLE id;
	ULONGLONG area;
};
static NOTIFY_LISTENER g_listener[50];
static int g_dwListener = 0;

static XSESSION_LOCAL_DETAILS xlive_session_details;

static bool xlive_invite_to_game = false;

static CRITICAL_SECTION d_lock;

static char* xlive_preferred_network_adapter_name = NULL;
EligibleAdapter xlive_network_adapter;

INT GetNetworkAdapter()
{
	// Declare and initialize variables
	DWORD dwRetVal = 0;

	// Set the flags to pass to GetAdaptersAddresses
	ULONG flags = GAA_FLAG_INCLUDE_PREFIX;

	// IPv4
	ULONG family = AF_INET;

	PIP_ADAPTER_ADDRESSES pAddresses = NULL;
	// Allocate a 15 KB buffer to start with.
	ULONG outBufLen = 15000;
	ULONG Iterations = 0;

	PIP_ADAPTER_ADDRESSES pCurrAddresses = NULL;
	PIP_ADAPTER_UNICAST_ADDRESS pUnicast = NULL;

	INT result = ERROR_UNIDENTIFIED_ERROR;

	do {
		pAddresses = (IP_ADAPTER_ADDRESSES*)HeapAlloc(GetProcessHeap(), 0, (outBufLen));
		if (pAddresses == NULL) {
			addDebugText("Memory allocation failed for IP_ADAPTER_ADDRESSES struct");
			dwRetVal = ERROR_NOT_ENOUGH_MEMORY;
			break;
		}

		dwRetVal = GetAdaptersAddresses(family, flags, NULL, pAddresses, &outBufLen);

		if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
			HeapFree(GetProcessHeap(), 0, pAddresses);
			pAddresses = NULL;
		}
		else {
			break;
		}

		Iterations++;
		// 3 attempts max
	} while ((dwRetVal == ERROR_BUFFER_OVERFLOW) && (Iterations < 3));

	if (dwRetVal == NO_ERROR) {
		std::vector<EligibleAdapter*> eligible_adapters;

		// If successful, output some information from the data we received
		pCurrAddresses = pAddresses;
		while (pCurrAddresses) {

			if (pCurrAddresses->OperStatus == 1) {

				//addDebugText("\tAdapter name: %s", pCurrAddresses->AdapterName);

				pUnicast = pCurrAddresses->FirstUnicastAddress;
				
				for (int i = 0; pUnicast != NULL; i++)
				{
					if (pUnicast->Address.lpSockaddr->sa_family == AF_INET)
					{
						sockaddr_in *sa_in = (sockaddr_in *)pUnicast->Address.lpSockaddr;
						ULONG dwMask = 0;
						dwRetVal = ConvertLengthToIpv4Mask(pUnicast->OnLinkPrefixLength, &dwMask);
						if (dwRetVal == NO_ERROR) {
							EligibleAdapter *ea = new EligibleAdapter;
							ea->name = pCurrAddresses->AdapterName;
							ea->unicastHAddr = ntohl(((sockaddr_in *)pUnicast->Address.lpSockaddr)->sin_addr.s_addr);
							ea->unicastHMask = ntohl(dwMask);
							ea->minLinkSpeed = pCurrAddresses->ReceiveLinkSpeed;
							if (pCurrAddresses->TransmitLinkSpeed < pCurrAddresses->ReceiveLinkSpeed)
								ea->minLinkSpeed = pCurrAddresses->TransmitLinkSpeed;
							ea->hasDnsServer = pCurrAddresses->FirstDnsServerAddress ? TRUE : FALSE;
							eligible_adapters.push_back(ea);
						}
					}
					pUnicast = pUnicast->Next;
				}
			}

			pCurrAddresses = pCurrAddresses->Next;
		}

		EligibleAdapter* chosenAdapter = NULL;

		for (EligibleAdapter* ea : eligible_adapters) {
			if (xlive_preferred_network_adapter_name && strncmp(ea->name, xlive_preferred_network_adapter_name, 50) == 0) {
				chosenAdapter = ea;
				break;
			}
			if (ea->unicastHAddr == INADDR_LOOPBACK || ea->unicastHAddr == INADDR_BROADCAST || ea->unicastHAddr == INADDR_NONE)
				continue;
			if (!chosenAdapter) {
				chosenAdapter = ea;
				continue;
			}
			if ((ea->hasDnsServer && !chosenAdapter->hasDnsServer) ||
				(ea->minLinkSpeed > chosenAdapter->minLinkSpeed)) {
				chosenAdapter = ea;
			}
		}

		if (chosenAdapter) {
			memcpy_s(&xlive_network_adapter, sizeof(EligibleAdapter), chosenAdapter, sizeof(EligibleAdapter));
			xlive_network_adapter.hBroadcast = ~(xlive_network_adapter.unicastHMask) | xlive_network_adapter.unicastHAddr;

			unsigned int adapter_name_buflen = strnlen_s(chosenAdapter->name, 49) + 1;
			xlive_network_adapter.name = (char*)malloc(adapter_name_buflen);
			memcpy_s(xlive_network_adapter.name, adapter_name_buflen, chosenAdapter->name, adapter_name_buflen);
			xlive_network_adapter.name[adapter_name_buflen - 1] = 0;

			result = ERROR_SUCCESS;
		}
		else {
			result = ERROR_NETWORK_UNREACHABLE;
		}

		for (EligibleAdapter* ea : eligible_adapters) {
			delete ea;
		}
		eligible_adapters.clear();
	}
	else {
		char errorMsg[400];
		snprintf(errorMsg, 400, "Call to GetAdaptersAddresses failed with error: %d", dwRetVal);
		addDebugText(errorMsg);
		if (dwRetVal == ERROR_NO_DATA)
			addDebugText("No addresses were found for the requested parameters");
		else {
			LPSTR lpMsgBuf = NULL;
			if (FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER |
				FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				NULL, dwRetVal, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
				// Default language
				(LPSTR)&lpMsgBuf, 0, NULL)) {
				snprintf(errorMsg, 400, "Error: %s", lpMsgBuf);
				addDebugText(errorMsg);
				LocalFree(lpMsgBuf);
			}
		}
		result = ERROR_UNIDENTIFIED_ERROR;
	}

	if (pAddresses) {
		HeapFree(GetProcessHeap(), 0, pAddresses);
	}

	if (result != ERROR_SUCCESS) {
		xlive_network_adapter.name = NULL;
		xlive_network_adapter.unicastHAddr = INADDR_LOOPBACK;
		xlive_network_adapter.unicastHMask = 0;
		xlive_network_adapter.hBroadcast = INADDR_BROADCAST;
		xlive_network_adapter.hasDnsServer = FALSE;
		xlive_network_adapter.minLinkSpeed = 0;
	}

	return result;
}

void CreateLocalUser()
{
	INT error_network_adapter = GetNetworkAdapter();

	XNADDR *pAddr = &xlive_local_users[0].pxna;

	unsigned long resolvedAddr;
	if ((resolvedAddr = xlive_network_adapter.unicastHAddr) == INADDR_NONE) {//inet_addr("192.168.0.6")
		return;
	}

	//DWORD user_id = 0x6061B52F;
	DWORD user_id = rand();
	DWORD mac_fix = 0x00131000;

	pAddr->ina.s_addr = htonl(resolvedAddr);
	pAddr->wPortOnline = htons(xlive_base_port);
	pAddr->inaOnline.s_addr = user_id << 8;

	memset(&(pAddr->abEnet), 0, 6);
	memset(&(pAddr->abOnline), 0, 6);

	memcpy(&(pAddr->abEnet), &user_id, 4);
	memcpy(&(pAddr->abOnline), &user_id, 4);

	memcpy((BYTE*)&(pAddr->abEnet) + 3, (BYTE*)&mac_fix + 1, 3);
	memcpy((BYTE*)&(pAddr->abOnline) + 17, (BYTE*)&mac_fix + 1, 3);

	xlive_local_users[0].pina.s_addr = pAddr->inaOnline.s_addr;

	xlive_local_users[0].bValid = TRUE;

	//CreateUser(pAddr, TRUE);
}


void Check_Overlapped(PXOVERLAPPED pOverlapped)
{
	if (!pOverlapped)
		return;

	if (pOverlapped->hEvent) {
		SetEvent(pOverlapped->hEvent);
	}

	if (pOverlapped->pCompletionRoutine) {
		pOverlapped->pCompletionRoutine(pOverlapped->InternalLow, pOverlapped->InternalHigh, pOverlapped->dwCompletionContext);
	}
}


BOOL XLivepIsPropertyIdValid(DWORD dwPropertyId, BOOL a2)
{
	return !(dwPropertyId & X_PROPERTY_SCOPE_MASK)
		|| dwPropertyId == X_PROPERTY_RANK
		|| dwPropertyId == X_PROPERTY_SESSION_ID
		|| dwPropertyId == X_PROPERTY_GAMER_ZONE
		|| dwPropertyId == X_PROPERTY_GAMER_COUNTRY
		|| dwPropertyId == X_PROPERTY_GAMER_LANGUAGE
		|| dwPropertyId == X_PROPERTY_GAMER_RATING
		|| dwPropertyId == X_PROPERTY_GAMER_MU
		|| dwPropertyId == X_PROPERTY_GAMER_SIGMA
		|| dwPropertyId == X_PROPERTY_GAMER_PUID
		|| dwPropertyId == X_PROPERTY_AFFILIATE_SCORE
		|| dwPropertyId == X_PROPERTY_RELATIVE_SCORE
		|| dwPropertyId == X_PROPERTY_SESSION_TEAM
		|| !a2 && dwPropertyId == X_PROPERTY_GAMER_HOSTNAME;
}


// #472
VOID WINAPI XCustomSetAction(DWORD dwActionIndex, LPCWSTR szActionText, DWORD dwFlags)
{
	TRACE_FX();
}

// #473
BOOL WINAPI XCustomGetLastActionPress(DWORD *pdwUserIndex, DWORD *pdwActionIndex, XUID *pXuid)
{
	TRACE_FX();
	return FALSE;
}

// #651
BOOL WINAPI XNotifyGetNext(HANDLE hNotification, DWORD dwMsgFilter, PDWORD pdwId, PULONG_PTR pParam)
{
	TRACE_FX();

	EnterCriticalSection(&d_lock);

	ResetEvent(hNotification);

	BOOL result = FALSE;
	int noteId = 0;
	for (; noteId < g_dwListener; noteId++) {
		if (g_listener[noteId].id == hNotification)
			break;
	}
	if (noteId == g_dwListener) {
		noteId = -1;
	}
	else {
		if (g_listener[noteId].area & XNOTIFY_SYSTEM) {

			*pParam = 0x00000000;
			
			for (int i = 0; i < XLIVE_LOCAL_USER_COUNT; i++) {
				if (xlive_users_info_changed[i]) {
					xlive_users_info_changed[i] = FALSE;
					*pParam |= 1 << i;
				}
			}

			if (*pParam) {
				*pdwId = XN_SYS_SIGNINCHANGED;
				result = TRUE;
			}
		}
		else if (g_listener[noteId].area & XNOTIFY_SYSTEM) {

			//*pParam = XONLINE_S_LOGON_CONNECTION_ESTABLISHED;
			*pParam = XONLINE_S_LOGON_DISCONNECTED;

			if (*pParam) {
				*pdwId = XN_LIVE_CONNECTIONCHANGED;
				result = TRUE;
			}
		}
	}

	if (result)
		SetEvent(hNotification);
	LeaveCriticalSection(&d_lock);
	return result;
}

// #653
DWORD WINAPI XNotifyDelayUI(ULONG ulMilliSeconds)
{
	TRACE_FX();
	return ERROR_SUCCESS;
}

// #1082
DWORD WINAPI XGetOverlappedExtendedError(PXOVERLAPPED pOverlapped)
{
	TRACE_FX();
	if (!pOverlapped) {
		return ERROR_INVALID_PARAMETER;
	}

	return pOverlapped->dwExtendedError;
}

// #1083
DWORD WINAPI XGetOverlappedResult(PXOVERLAPPED pOverlapped, LPDWORD pdwResult, BOOL bWait)
{
	TRACE_FX();
	if (bWait) {
		while (pOverlapped->InternalLow == ERROR_IO_INCOMPLETE) {
		}
	}

	if (pdwResult) {
		*pdwResult = pOverlapped->InternalHigh;
	}

	return pOverlapped->InternalLow;
}

// #5000
HRESULT WINAPI XLiveInitialize(XLIVE_INITIALIZE_INFO *pPii)
{
	TRACE_FX();
	//TODO startup flags.
	while (FALSE && !IsDebuggerPresent())
		Sleep(500L);

	srand((unsigned int)time(NULL));

	if (pPii->pszAdapterName && pPii->pszAdapterName[0]) {
		unsigned int adapter_name_buflen = strnlen_s(pPii->pszAdapterName, 49) + 1;
		xlive_preferred_network_adapter_name = (char*)malloc(adapter_name_buflen);
		memcpy_s(xlive_preferred_network_adapter_name, adapter_name_buflen, pPii->pszAdapterName, adapter_name_buflen);
		xlive_preferred_network_adapter_name[adapter_name_buflen-1] = 0;
	}

	memset(&xlive_network_adapter, 0x00, sizeof(EligibleAdapter));

	for (int i = 0; i < XLIVE_LOCAL_USER_COUNT; i++) {
		xlive_users_info[i] = (XUSER_SIGNIN_INFO*)malloc(sizeof(XUSER_SIGNIN_INFO));
		memset(xlive_users_info[i], 0, sizeof(XUSER_SIGNIN_INFO));
		xlive_users_info_changed[i] = FALSE;
	}

	memset(&xlive_session_details, 0, sizeof(XSESSION_LOCAL_DETAILS));

	InitializeCriticalSection(&d_lock);

	wchar_t mutex_name[40];
	DWORD mutex_last_error;
	HANDLE mutex = NULL;
	do {
		if (mutex)
			mutex_last_error = CloseHandle(mutex);
		xlive_base_port += 1000;
		if (xlive_base_port > 65000) {
			xlive_netsocket_abort = TRUE;
			xlive_base_port = 1000;
			break;
		}
		swprintf(mutex_name, 40, L"Global\\XLLNBasePort#%hd", xlive_base_port);
		mutex = CreateMutexW(0, TRUE, mutex_name);
		mutex_last_error = GetLastError();
	} while (mutex_last_error != ERROR_SUCCESS);

	char debugText[50];
	snprintf(debugText, 50, "XLive Base Port %hd.", xlive_base_port);
	addDebugText(debugText);

	CreateLocalUser();

	//TODO If the title's graphics system has not yet been initialized, D3D will be passed in XLiveOnCreateDevice(...).
	INT error_XRender = InitXRender(pPii);

	return S_OK;
}

// #5001
HRESULT WINAPI XLiveInput(XLIVE_INPUT_INFO *pPii)
{
	TRACE_FX();
	pPii->fHandled = FALSE;
	return S_OK;
}

// #5003
VOID WINAPI XLiveUninitialize()
{
	TRACE_FX();
	INT error_XRender = UninitXRender();
}

// #5007
HRESULT WINAPI XLiveOnResetDevice(void *pD3DPP)
{
	TRACE_FX();
	D3DPRESENT_PARAMETERS *pD3DPP2 = (D3DPRESENT_PARAMETERS*)pD3DPP;
	//ID3D10Device *pD3DPP2 = (ID3D10Device*)pD3DPP;
	return S_OK;
}

// #5010: This function is deprecated.
HRESULT WINAPI XLiveRegisterDataSection(int a1, int a2, int a3)
{
	TRACE_FX();
	return ERROR_SUCCESS;
	//if (XLivepGetTitleXLiveVersion() < 0x20000000)
	return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

// #5011: This function is deprecated.
HRESULT WINAPI XLiveUnregisterDataSection(int a1)
{
	TRACE_FX();
	return ERROR_SUCCESS;
	//if (XLivepGetTitleXLiveVersion() < 0x20000000)
	return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

// #5016
HRESULT WINAPI XLivePBufferAllocate(DWORD dwSize, XLIVE_PROTECTED_BUFFER **pxebBuffer)
{
	TRACE_FX();
	if (!dwSize)
		return E_INVALIDARG;
	if (!pxebBuffer)
		return E_POINTER;
	if (dwSize + 4 < dwSize)
		//Overflow experienced.
		return E_UNEXPECTED;

	HANDLE hHeap = GetProcessHeap();
	*pxebBuffer = (XLIVE_PROTECTED_BUFFER*)HeapAlloc(hHeap, HEAP_ZERO_MEMORY, dwSize + 4);
	if (!*pxebBuffer)
		return E_OUTOFMEMORY;

	(*pxebBuffer)->dwSize = dwSize;

	return S_OK;
}

// #5018
HRESULT WINAPI XLivePBufferGetByte(XLIVE_PROTECTED_BUFFER *xebBuffer, DWORD dwOffset, BYTE *pucValue)
{
	TRACE_FX();
	if (!xebBuffer)
		return E_POINTER;
	if (!pucValue)
		return E_POINTER;
	if (dwOffset + 4 < dwOffset)
		//Overflow experienced.
		return E_UNEXPECTED;
	if (dwOffset >= xebBuffer->dwSize)
		return E_UNEXPECTED;

	*pucValue = ((BYTE*)&xebBuffer->bData)[dwOffset];
	return S_OK;
}

// #5019
HRESULT WINAPI XLivePBufferSetByte(XLIVE_PROTECTED_BUFFER *xebBuffer, DWORD dwOffset, BYTE ucValue)
{
	TRACE_FX();
	if (!xebBuffer)
		return E_POINTER;
	if (dwOffset + 4 < dwOffset)
		//Overflow experienced.
		return E_UNEXPECTED;
	if (dwOffset >= xebBuffer->dwSize)
		return E_UNEXPECTED;

	((BYTE*)&xebBuffer->bData)[dwOffset] = ucValue;

	return S_OK;
}

// #5022
HRESULT WINAPI XLiveGetUpdateInformation(PXLIVEUPDATE_INFORMATION pXLiveUpdateInfo)
{
	TRACE_FX();
	if (!pXLiveUpdateInfo)
		return E_POINTER;
	if (pXLiveUpdateInfo->cbSize != sizeof(XLIVEUPDATE_INFORMATION))
		return HRESULT_FROM_WIN32(ERROR_INVALID_USER_BUFFER);//0x800706F8;
	// No update?
	return S_FALSE;
}

// #5024
HRESULT WINAPI XLiveUpdateSystem(LPCWSTR lpszRelaunchCmdLine)
{
	TRACE_FX();
	return S_OK;
	// No update?
	return S_FALSE;
}

// #5026
HRESULT WINAPI XLiveSetSponsorToken(LPCWSTR pwszToken, DWORD dwTitleId)
{
	TRACE_FX();
	if (!pwszToken || wcsnlen_s(pwszToken, 30) != 29)
		return E_INVALIDARG;
	return S_OK;
}

// #5028
DWORD WINAPI XLiveLoadLibraryEx(LPCWSTR pszModuleFileName, HINSTANCE *phModule, DWORD dwFlags)
{
	TRACE_FX();
	if (!pszModuleFileName)
		return E_POINTER;
	if (!*pszModuleFileName)
		return E_INVALIDARG;
	if (!phModule)
		return E_INVALIDARG;

	HINSTANCE hInstance = LoadLibraryExW(pszModuleFileName, NULL, dwFlags);
	if (!hInstance)
		return E_INVALIDARG;

	*phModule = hInstance;
	return S_OK;
}

// #5029
HRESULT WINAPI XLiveFreeLibrary(HMODULE hModule)
{
	TRACE_FX();
	if (!hModule)
		return E_INVALIDARG;
	if (!FreeLibrary(hModule)) {
		signed int last_error = GetLastError();
		if (last_error > 0)
			last_error = (unsigned __int16)last_error | 0x80070000;
		return last_error;
	}
	return S_OK;
}

// #5030
BOOL WINAPI XLivePreTranslateMessage(const LPMSG lpMsg)
{
	TRACE_FX();
	if (!lpMsg)
		return FALSE;
	return FALSE;
	//return TRUE;
}

// #5212
DWORD WINAPI XShowCustomPlayerListUI(
	DWORD dwUserIndex,
	DWORD dwFlags,
	LPCWSTR pszTitle,
	LPCWSTR pszDescription,
	CONST BYTE *pbImage,
	DWORD cbImage,
	CONST XPLAYERLIST_USER *rgPlayers,
	DWORD cPlayers,
	CONST XPLAYERLIST_BUTTON *pXButton,
	CONST XPLAYERLIST_BUTTON *pYButton,
	XPLAYERLIST_RESULT *pResult,
	XOVERLAPPED *pOverlapped)
{
	TRACE_FX();
	return ERROR_SUCCESS;
}

// #5215
DWORD WINAPI XShowGuideUI(DWORD dwUserIndex)
{
	TRACE_FX();
	ShowXLLN(XLLN_SHOW_HOME);
	return ERROR_SUCCESS;
}

// #5251
BOOL WINAPI XCloseHandle(HANDLE hObject)
{
	TRACE_FX();
	if (!hObject || (DWORD)hObject == -1) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	if (!CloseHandle(hObject)) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	return TRUE;
}

// #5252
DWORD WINAPI XShowGamerCardUI(DWORD dwUserIndex, XUID XuidPlayer)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;

	return ERROR_SUCCESS;
}

// #5254
DWORD WINAPI XCancelOverlapped(PXOVERLAPPED pOverlapped)
{
	TRACE_FX();
	if (!pOverlapped)
		return ERROR_INVALID_PARAMETER;
	//TODO
	return ERROR_SUCCESS;
}

// #5256
DWORD WINAPI XEnumerate(HANDLE hEnum, PVOID pvBuffer, DWORD cbBuffer, PDWORD pcItemsReturned, PXOVERLAPPED pXOverlapped)
{
	TRACE_FX();
	if (!hEnum)
		return ERROR_INVALID_PARAMETER;
	if (!pvBuffer)
		return ERROR_INVALID_PARAMETER;
	if (!cbBuffer)
		return ERROR_INVALID_PARAMETER;
	if (pcItemsReturned && pXOverlapped)
		return ERROR_INVALID_PARAMETER;
	if (!pcItemsReturned && !pXOverlapped)
		return ERROR_INVALID_PARAMETER;

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5257
HRESULT WINAPI XLiveManageCredentials(LPCWSTR lpszLiveIdName, LPCWSTR lpszLiveIdPassword, DWORD dwCredFlags, PXOVERLAPPED pXOverlapped)
{
	TRACE_FX();
	if (dwCredFlags & XLMGRCREDS_FLAG_SAVE && dwCredFlags & XLMGRCREDS_FLAG_DELETE || !(dwCredFlags & XLMGRCREDS_FLAG_SAVE) && !(dwCredFlags & XLMGRCREDS_FLAG_DELETE))
		return E_INVALIDARG;
	if (!lpszLiveIdName || !*lpszLiveIdName)
		return E_INVALIDARG;
	if (dwCredFlags & XLMGRCREDS_FLAG_SAVE && (!lpszLiveIdPassword || !*lpszLiveIdPassword))
		return E_INVALIDARG;

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return S_OK;
}

// #5258
HRESULT WINAPI XLiveSignout(PXOVERLAPPED  pXOverlapped)
{
	TRACE_FX();

	XLLNLogout(0);

	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return S_OK;
}

// #5259
HRESULT WINAPI XLiveSignin(PWSTR pszLiveIdName, PWSTR pszLiveIdPassword, DWORD dwFlags, PXOVERLAPPED pXOverlapped)
{
	TRACE_FX();
	if (!pszLiveIdName || !*pszLiveIdName)
		return E_INVALIDARG;
	if (!pszLiveIdPassword || !*pszLiveIdPassword)
		return E_INVALIDARG;

	if (dwFlags & XLSIGNIN_FLAG_SAVECREDS) {

	}
	//XLSIGNIN_FLAG_ALLOWTITLEUPDATES XLSIGNIN_FLAG_ALLOWSYSTEMUPDATES

	XLLNLogin(0, FALSE, 0, 0);

	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return S_OK;
}

// #5260
DWORD WINAPI XShowSigninUI(DWORD cPanes, DWORD dwFlags)
{
	TRACE_FX();
	if (!(!(dwFlags & 0x40FFFC) && (!(dwFlags & XSSUI_FLAGS_LOCALSIGNINONLY) || !(dwFlags & XSSUI_FLAGS_SHOWONLYONLINEENABLED)))) {
		return ERROR_INVALID_PARAMETER;
	}

	// Number of users to sign in.
	if (!cPanes)
		return ERROR_INVALID_PARAMETER;

	ShowXLLN(XLLN_SHOW_LOGIN);

	//TODO
	return ERROR_SUCCESS;
	// no users signed in with multiplayer privilege. if XSSUI_FLAGS_SHOWONLYONLINEENABLED is flagged?
	return ERROR_FUNCTION_FAILED;
}

// #5261
DWORD WINAPI XUserGetXUID(DWORD dwUserIndex, XUID *pxuid)
{
	TRACE_FX();
	if (!pxuid)
		return ERROR_INVALID_PARAMETER;
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;

	if (xlive_users_info[dwUserIndex]->UserSigninState & (eXUserSigninState_SignedInLocally | eXUserSigninState_SignedInToLive)) {
		*pxuid = xlive_users_info[dwUserIndex]->xuid;
		return ERROR_SUCCESS;
	}
	return ERROR_NO_SUCH_USER;
}

// #5262
XUSER_SIGNIN_STATE WINAPI XUserGetSigninState(DWORD dwUserIndex)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return eXUserSigninState_NotSignedIn;
	return xlive_users_info[dwUserIndex]->UserSigninState;
}

// #5263
DWORD WINAPI XUserGetName(DWORD dwUserIndex, LPSTR szUserName, DWORD cchUserName)
{
	TRACE_FX();
	if (!szUserName)
		return ERROR_INVALID_PARAMETER;
	if (!cchUserName)
		return ERROR_INVALID_PARAMETER;
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;

	if (cchUserName > XUSER_NAME_SIZE)
		cchUserName = XUSER_NAME_SIZE;

	memcpy(szUserName, xlive_users_info[dwUserIndex]->szUserName, cchUserName);
	return ERROR_SUCCESS;
}

// #5265
DWORD WINAPI XUserCheckPrivilege(DWORD dwUserIndex, XPRIVILEGE_TYPE PrivilegeType, PBOOL pfResult)
{
	TRACE_FX();
	if (!pfResult)
		return ERROR_INVALID_PARAMETER;
	*pfResult = FALSE;
	if (PrivilegeType != XPRIVILEGE_MULTIPLAYER_SESSIONS &&
		PrivilegeType != XPRIVILEGE_COMMUNICATIONS &&
		PrivilegeType != XPRIVILEGE_COMMUNICATIONS_FRIENDS_ONLY &&
		PrivilegeType != XPRIVILEGE_PROFILE_VIEWING &&
		PrivilegeType != XPRIVILEGE_PROFILE_VIEWING_FRIENDS_ONLY &&
		PrivilegeType != XPRIVILEGE_USER_CREATED_CONTENT &&
		PrivilegeType != XPRIVILEGE_USER_CREATED_CONTENT_FRIENDS_ONLY &&
		PrivilegeType != XPRIVILEGE_PURCHASE_CONTENT &&
		PrivilegeType != XPRIVILEGE_PRESENCE &&
		PrivilegeType != XPRIVILEGE_PRESENCE_FRIENDS_ONLY &&
		PrivilegeType != XPRIVILEGE_TRADE_CONTENT &&
		PrivilegeType != XPRIVILEGE_VIDEO_COMMUNICATIONS &&
		PrivilegeType != XPRIVILEGE_VIDEO_COMMUNICATIONS_FRIENDS_ONLY &&
		PrivilegeType != XPRIVILEGE_MULTIPLAYER_DEDICATED_SERVER)
		return ERROR_INVALID_PARAMETER;
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;

	if (TRUE)//TODO
		*pfResult = TRUE;

	return ERROR_SUCCESS;
}

// #5270: Requires XNotifyGetNext to process the listener.
HANDLE WINAPI XNotifyCreateListener(ULONGLONG qwAreas)
{
	TRACE_FX();
	if (HIDWORD(qwAreas) | qwAreas & 0xFFFFFF10)
		return NULL;

	HANDLE g_dwFakeListener = CreateMutex(NULL, NULL, NULL);

	g_listener[g_dwListener].id = g_dwFakeListener;
	g_listener[g_dwListener].area = qwAreas;
	g_dwListener++;

	SetEvent(g_dwFakeListener);
	return g_dwFakeListener;
}

// #5276
VOID WINAPI XUserSetProperty(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, CONST VOID *pvValue)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return;// ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return;// ERROR_NOT_LOGGED_ON;
	if (!cbValue)
		return;// ERROR_INVALID_PARAMETER;
	if (!pvValue)
		return;// ERROR_INVALID_PARAMETER;
	if (!XLivepIsPropertyIdValid(dwPropertyId, TRUE))
		return;

	//TODO
}

// #5277
VOID WINAPI XUserSetContext(DWORD dwUserIndex, DWORD dwContextId, DWORD dwContextValue)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return;// ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return;// ERROR_NOT_LOGGED_ON;

	if (dwContextId == X_CONTEXT_PRESENCE) {

	}
	else if (dwContextId == X_CONTEXT_GAME_TYPE) {
		xlive_session_details.dwGameType = dwContextValue;
	}
	else if (dwContextId == X_CONTEXT_GAME_MODE) {
		xlive_session_details.dwGameMode = dwContextValue;
	}
	else if (dwContextId == X_CONTEXT_SESSION_JOINABLE) {

	}
}

// #5278
DWORD WINAPI XUserWriteAchievements(DWORD dwNumAchievements, CONST XUSER_ACHIEVEMENT *pAchievements, PXOVERLAPPED pXOverlapped)
{
	TRACE_FX();
	if (!dwNumAchievements)
		return ERROR_INVALID_PARAMETER;
	if (!pAchievements)
		return ERROR_INVALID_PARAMETER;

	if (pAchievements->dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5280
DWORD WINAPI XUserCreateAchievementEnumerator(DWORD dwTitleId, DWORD dwUserIndex, XUID xuid, DWORD dwDetailFlags, DWORD dwStartingIndex, DWORD cItem, PDWORD pcbBuffer, PHANDLE phEnum)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (cItem > XACHIEVEMENT_MAX_COUNT)
		return ERROR_INVALID_PARAMETER;
	if (dwStartingIndex >= XACHIEVEMENT_MAX_COUNT)
		return ERROR_INVALID_PARAMETER;
	if (dwStartingIndex + cItem < dwStartingIndex || dwStartingIndex + cItem >= XACHIEVEMENT_MAX_COUNT)
		return ERROR_INVALID_PARAMETER;
	if (!dwDetailFlags || (dwDetailFlags != XACHIEVEMENT_DETAILS_ALL && dwDetailFlags & ~(XACHIEVEMENT_DETAILS_LABEL | XACHIEVEMENT_DETAILS_DESCRIPTION | XACHIEVEMENT_DETAILS_UNACHIEVED | XACHIEVEMENT_DETAILS_TFC)))
		return ERROR_INVALID_PARAMETER;
	if (!pcbBuffer)
		return ERROR_INVALID_PARAMETER;
	if (!phEnum)
		return ERROR_INVALID_PARAMETER;

	if (xuid == INVALID_XUID) {
		//enumerate the local signed-in gamer's achievements.
	}

	for (DWORD i = dwStartingIndex; i < dwStartingIndex + cItem; i++) {
		//?
	}

	*pcbBuffer = cItem * sizeof(XACHIEVEMENT_DETAILS);
	*phEnum = CreateMutex(NULL, NULL, NULL);

	return ERROR_SUCCESS;
}

// #5300
DWORD WINAPI XSessionCreate(DWORD dwFlags, DWORD dwUserIndex, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, ULONGLONG *pqwSessionNonce, PXSESSION_INFO pSessionInfo, PXOVERLAPPED pXOverlapped, PHANDLE phEnum)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!pqwSessionNonce)
		return ERROR_INVALID_PARAMETER;
	if (!pSessionInfo)
		return ERROR_INVALID_PARAMETER;
	if (!phEnum)
		return ERROR_INVALID_PARAMETER;
	if (dwFlags & ~(XSESSION_CREATE_USES_MASK | XSESSION_CREATE_MODIFIERS_MASK | 0x1000))//FIXME unknown macro or their mistake?
		return ERROR_INVALID_PARAMETER;
	if (dwFlags & XSESSION_CREATE_USES_MATCHMAKING && !(dwFlags & XSESSION_CREATE_USES_PEER_NETWORK))
		return ERROR_INVALID_PARAMETER;
	if (dwFlags & XSESSION_CREATE_USES_ARBITRATION && !(dwFlags & XSESSION_CREATE_USES_STATS))
		return ERROR_INVALID_PARAMETER;
	if (dwFlags & XSESSION_CREATE_USES_ARBITRATION && !(dwFlags & XSESSION_CREATE_USES_PEER_NETWORK))
		return ERROR_INVALID_PARAMETER;
	if (dwFlags & XSESSION_CREATE_HOST && !(dwFlags & (XSESSION_CREATE_USES_PEER_NETWORK | XSESSION_CREATE_USES_STATS | XSESSION_CREATE_USES_MATCHMAKING)))
		return ERROR_INVALID_PARAMETER;
	if (dwFlags & XSESSION_CREATE_MODIFIERS_MASK) {
		if (!(dwFlags & (XSESSION_CREATE_USES_PRESENCE | XSESSION_CREATE_USES_MATCHMAKING))) {
			return ERROR_INVALID_PARAMETER;
		}
		if (!(dwFlags & XSESSION_CREATE_USES_PRESENCE) && (dwFlags & XSESSION_CREATE_USES_MATCHMAKING) && (dwFlags & XSESSION_CREATE_MODIFIERS_MASK) != (dwFlags & XSESSION_CREATE_JOIN_IN_PROGRESS_DISABLED)) {
			return ERROR_INVALID_PARAMETER;
		}
		if ((dwFlags & XSESSION_CREATE_JOIN_VIA_PRESENCE_DISABLED) && (dwFlags & XSESSION_CREATE_JOIN_VIA_PRESENCE_FRIENDS_ONLY)) {
			return ERROR_INVALID_PARAMETER;
		}
	}

	*phEnum = CreateMutex(NULL, NULL, NULL);

	// local cache
	xlive_session_details.dwUserIndexHost = dwUserIndex;//XUSER_INDEX_NONE ?

	// already filled - SetContext
	//xlive_session_details.dwGameType = 0;
	//xlive_session_details.dwGameMode = 0;

	xlive_session_details.dwFlags = dwFlags;

	xlive_session_details.dwMaxPublicSlots = dwMaxPublicSlots;
	xlive_session_details.dwMaxPrivateSlots = dwMaxPrivateSlots;
	xlive_session_details.dwAvailablePublicSlots = dwMaxPublicSlots;
	xlive_session_details.dwAvailablePrivateSlots = dwMaxPrivateSlots;

	xlive_session_details.dwActualMemberCount = 0;
	xlive_session_details.dwReturnedMemberCount = 0;

	xlive_session_details.eState = XSESSION_STATE_LOBBY;
	xlive_session_details.qwNonce = *pqwSessionNonce;

	//xlive_session_details.sessionInfo = *pSessionInfo; //check this.

	
	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5303
DWORD WINAPI XStringVerify(DWORD dwFlags, const CHAR *szLocale, DWORD dwNumStrings, const STRING_DATA *pStringData, DWORD cbResults, STRING_VERIFY_RESPONSE *pResults, XOVERLAPPED *pXOverlapped)
{
	TRACE_FX();
	if (dwFlags)
		// Not implemented.
		return ERROR_INVALID_PARAMETER;
	if (!szLocale)
		return ERROR_INVALID_PARAMETER;
	if (strlen(szLocale) >= XSTRING_MAX_LENGTH)
		return ERROR_INVALID_PARAMETER;
	if (dwNumStrings > XSTRING_MAX_STRINGS)
		return ERROR_INVALID_PARAMETER;
	if (!pStringData)
		return ERROR_INVALID_PARAMETER;
	if (!pResults)
		return ERROR_INVALID_PARAMETER;

	pResults->wNumStrings = (WORD)dwNumStrings;
	pResults->pStringResult = (HRESULT*)((BYTE*)pResults + sizeof(STRING_VERIFY_RESPONSE));

	for (int lcv = 0; lcv < (WORD)dwNumStrings; lcv++)
		pResults->pStringResult[lcv] = S_OK;
	
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5305
DWORD WINAPI XStorageUploadFromMemory(DWORD dwUserIndex, const WCHAR *wszServerPath, DWORD dwBufferSize, const BYTE *pbBuffer, XOVERLAPPED *pXOverlapped)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!wszServerPath)
		return ERROR_INVALID_PARAMETER;
	if (!dwBufferSize)
		return ERROR_INVALID_PARAMETER;
	if (!pbBuffer)
		return ERROR_INVALID_PARAMETER;

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5308
DWORD WINAPI XStorageDelete(DWORD dwUserIndex, const WCHAR *wszServerPath, XOVERLAPPED *pXOverlapped)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!wszServerPath)
		return ERROR_INVALID_PARAMETER;

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5310
DWORD WINAPI XOnlineStartup()
{
	TRACE_FX();
	if (!xlive_net_initialized)
		return ERROR_FUNCTION_FAILED;
	return ERROR_SUCCESS;
}

// #5312
DWORD WINAPI XFriendsCreateEnumerator(DWORD dwUserIndex, DWORD dwStartingIndex, DWORD dwFriendsToReturn, DWORD *pcbBuffer, HANDLE *ph)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (dwFriendsToReturn > XFRIENDS_MAX_C_RESULT)
		return ERROR_INVALID_PARAMETER;
	if (dwStartingIndex >= XFRIENDS_MAX_C_RESULT)
		return ERROR_INVALID_PARAMETER;
	if (dwStartingIndex + dwFriendsToReturn < dwStartingIndex || dwStartingIndex + dwFriendsToReturn >= XFRIENDS_MAX_C_RESULT)
		return ERROR_INVALID_PARAMETER;
	if (!pcbBuffer)
		return ERROR_INVALID_PARAMETER;
	if (!ph)
		return ERROR_INVALID_PARAMETER;

	*pcbBuffer = dwFriendsToReturn * sizeof(XCONTENT_DATA);
	*ph = CreateMutex(NULL, NULL, NULL);

	return ERROR_SUCCESS;
}

// #5315
DWORD WINAPI XInviteGetAcceptedInfo(DWORD dwUserIndex, XINVITE_INFO *pInfo)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!pInfo)
		return ERROR_INVALID_PARAMETER;

	//TODO
	if (xlive_invite_to_game) {
		xlive_invite_to_game = false;
		unsigned long resolvedAddr;
		if ((resolvedAddr = inet_addr("10.0.0.20")) == INADDR_NONE) {
			return ERROR;
		}

		pInfo->hostInfo.hostAddress.ina.s_addr = resolvedAddr;
		pInfo->hostInfo.hostAddress.wPortOnline = htons(2000);

		XUID host_xuid = 1234561000000032;
		pInfo->hostInfo.hostAddress.inaOnline.s_addr = 8192;

		DWORD user_id = host_xuid % 1000000000;
		DWORD mac_fix = 0x00131000;

		memset(&(pInfo->hostInfo.hostAddress.abEnet), 0, 6);
		memset(&(pInfo->hostInfo.hostAddress.abOnline), 0, 6);

		memcpy(&(pInfo->hostInfo.hostAddress.abEnet), &user_id, 4);
		memcpy(&(pInfo->hostInfo.hostAddress.abOnline), &user_id, 4);

		memcpy((BYTE*)&(pInfo->hostInfo.hostAddress.abEnet) + 3, (BYTE*)&mac_fix + 1, 3);
		memcpy((BYTE*)&(pInfo->hostInfo.hostAddress.abOnline) + 17, (BYTE*)&mac_fix + 1, 3);

		pInfo->fFromGameInvite = TRUE;
		pInfo->dwTitleID = 0x4D53080F;
		XNetCreateKey(&(pInfo->hostInfo.sessionID), &(pInfo->hostInfo.keyExchangeKey));
		pInfo->xuidInvitee = xlive_users_info[dwUserIndex]->xuid;
		pInfo->xuidInviter = host_xuid;

		return ERROR_SUCCESS;
	}
	return ERROR_FUNCTION_FAILED;
}

// #5331
DWORD WINAPI XUserReadProfileSettings(
	DWORD dwTitleId,
	DWORD dwUserIndex,
	DWORD dwNumSettingIds,
	const DWORD *pdwSettingIds,
	DWORD *pcbResults,
	PXUSER_READ_PROFILE_SETTING_RESULT pResults,
	PXOVERLAPPED pXOverlapped)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!dwNumSettingIds)
		return ERROR_INVALID_PARAMETER;
	if (!pdwSettingIds)
		return ERROR_INVALID_PARAMETER;
	if (!pcbResults)
		return ERROR_INVALID_PARAMETER;
	if (!pResults)
		return ERROR_INVALID_PARAMETER;

	//TODO
	if (*pcbResults < 1036) {
		*pcbResults = 1036;	// TODO: make correct calculation by IDs.
		return ERROR_INSUFFICIENT_BUFFER;
	}
	memset(pResults, 0, *pcbResults);
	pResults->dwSettingsLen = *pcbResults - sizeof(XUSER_PROFILE_SETTING);
	pResults->pSettings = (XUSER_PROFILE_SETTING *)pResults + sizeof(XUSER_PROFILE_SETTING);

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}

// #5344
DWORD WINAPI XStorageBuildServerPath(
	DWORD dwUserIndex,
	XSTORAGE_FACILITY StorageFacility,
	CONST void *pvStorageFacilityInfo,
	DWORD dwStorageFacilityInfoSize,
	LPCWSTR *pwszItemName,
	WCHAR *pwszServerPath,
	DWORD *pdwServerPathLength)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!pvStorageFacilityInfo && dwStorageFacilityInfoSize)
		return ERROR_INVALID_PARAMETER;
	if (!pwszItemName)
		return ERROR_INVALID_PARAMETER;
	if (!*pwszItemName)
		return ERROR_INVALID_PARAMETER;
	if (!pwszServerPath)
		return ERROR_INVALID_PARAMETER;
	if (!pdwServerPathLength)
		return ERROR_INVALID_PARAMETER;
	if (StorageFacility == XSTORAGE_FACILITY_GAME_CLIP && !pvStorageFacilityInfo)
		return ERROR_INVALID_PARAMETER;
	if (pvStorageFacilityInfo && dwStorageFacilityInfoSize < sizeof(XSTORAGE_FACILITY_GAME_CLIP))
		return ERROR_INVALID_PARAMETER;

	//TODO

	return ERROR_FUNCTION_FAILED;
	return ERROR_SUCCESS;
}

// #5345
DWORD WINAPI XStorageDownloadToMemory(
	DWORD dwUserIndex,
	const WCHAR *wszServerPath,
	DWORD dwBufferSize,
	const BYTE *pbBuffer,
	DWORD cbResults,
	XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS *pResults,
	XOVERLAPPED *pXOverlapped)
{
	TRACE_FX();
	if (dwUserIndex >= XLIVE_LOCAL_USER_COUNT)
		return ERROR_NO_SUCH_USER;
	if (xlive_users_info[dwUserIndex]->UserSigninState == eXUserSigninState_NotSignedIn)
		return ERROR_NOT_LOGGED_ON;
	if (!wszServerPath)
		return ERROR_INVALID_PARAMETER;
	if (!*wszServerPath)
		return ERROR_INVALID_PARAMETER;
	if (!dwBufferSize)
		return ERROR_INVALID_PARAMETER;
	if (!pbBuffer)
		return ERROR_INVALID_PARAMETER;
	if (cbResults != sizeof(XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS))
		return ERROR_INVALID_PARAMETER;
	if (!pResults)
		return ERROR_INVALID_PARAMETER;

	//TODO
	if (pXOverlapped) {
		//asynchronous

		pXOverlapped->InternalLow = ERROR_SUCCESS;
		pXOverlapped->InternalHigh = ERROR_SUCCESS;
		pXOverlapped->dwExtendedError = ERROR_SUCCESS;

		Check_Overlapped(pXOverlapped);

		return ERROR_IO_PENDING;
	}
	else {
		//synchronous
		//return result;
	}
	return ERROR_SUCCESS;
}
