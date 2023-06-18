#include "stdafx.h"
#include "Win32AppContext.h"
#include <assert.h>
#include <mCtrl\treelist.h>
#include <mCtrl\mditab.h>
#include "BatchImportDialog.h"
#include "Win32PluginManager.h"
#include <fstream>
#include <filesystem>

Win32AppContext::Win32AppContext(HINSTANCE hInstance, const std::string& baseDir)
	: mainWindow(hInstance), baseDir(baseDir), handlingMessages(false), messagePosted(false),
	gcMemoryLimit(0), gcMinAge(0)
{
	InitializeCriticalSection(&this->messageMutex);
	mainWindow.setContext(this);
}
Win32AppContext::~Win32AppContext()
{
	DeleteCriticalSection(&this->messageMutex);
}

void Win32AppContext::signalMainThread(EAppContextMsg message, void* args)
{
	bool notifyMainThread = true;
	EnterCriticalSection(&this->messageMutex);
	if (!this->messageQueue.empty() && messagePosted)
		notifyMainThread = false; //No need to send another notification.
	this->messageQueue.push_back(std::pair<EAppContextMsg, void*>(message, args));
	if (mainWindow.hDlg != NULL)
	{
		if (notifyMainThread)
			messagePosted = mainWindow.onAppContextMessageAsync() || messagePosted;
	}
	else
	{
		//The program is probably exiting, waiting for the last tasks to complete.
		//Since the main thread is inactive, it is sufficient to call handleMessages in a critical section.
		//(Note: Enter-/LeaveCriticalSection allows recursion)
		handleMessages();
	}
	LeaveCriticalSection(&this->messageMutex);
}
void Win32AppContext::handleMessages()
{
	std::vector<std::pair<EAppContextMsg, void*>> messageQueue;
	EnterCriticalSection(&this->messageMutex);
	if (handlingMessages)
	{
		//May be called in parallel only if the window is destructed (by signalMainThread).
		LeaveCriticalSection(&this->messageMutex);
		return;
	}
	messagePosted = false;
	handlingMessages = true;

	while (this->messageQueue.size() > 0) {
		this->messageQueue.swap(messageQueue);
		LeaveCriticalSection(&this->messageMutex);
		for (size_t i = 0; i < messageQueue.size(); ++i)
		{
			EAppContextMsg message = messageQueue[i].first;
			void* args = messageQueue[i].second;
			this->processMessage(message, args);
		}
		messageQueue.clear();
		EnterCriticalSection(&this->messageMutex);
	}
	handlingMessages = false;
	LeaveCriticalSection(&this->messageMutex);
}
bool Win32AppContext::processMessage(EAppContextMsg message, void* args)
{
	//switch ((EWin32AppContextMsg)message)
	//{
	//	default:
	return AppContext::processMessage(message, args);
	//}
}

void Win32AppContext::OnUpdateContainers(AssetsFileContextInfo* info)
{
	mainWindow.OnUpdateContainers(info);
}
void Win32AppContext::OnChangeAsset(AssetsFileContextInfo* pFile, pathid_t pathID, bool wasRemoved)
{
	AppContext::OnChangeAsset(pFile, pathID, wasRemoved);
	mainWindow.OnChangeAsset(pFile, pathID, wasRemoved);
}
void Win32AppContext::OnChangeBundleEntry(BundleFileContextInfo* pFile, size_t index)
{
	AppContext::OnChangeBundleEntry(pFile, index);
	mainWindow.OnChangeBundleEntry(pFile, index);
}
void Win32AppContext::OnUpdateDependencies(AssetsFileContextInfo* info, size_t from, size_t to)
{
	AppContext::OnUpdateDependencies(info, from, to);
	mainWindow.OnUpdateDependencies(info, from, to);
} //from/to: indices for info->references
std::shared_ptr<FileContextInfo> Win32AppContext::OnFileOpenAsBundle(std::shared_ptr<FileOpenTask> pTask, BundleFileContext* pContext,
	EBundleFileOpenStatus openStatus, unsigned int parentFileID, unsigned int directoryEntryIdx)
{
	std::shared_ptr<FileContextInfo> pInfo = AppContext::OnFileOpenAsBundle(pTask, pContext, openStatus, parentFileID, directoryEntryIdx);
	if (pInfo == nullptr)
		return nullptr;
	BundleFileContextInfo* pInfo_Bundle = reinterpret_cast<BundleFileContextInfo*>(pInfo.get());
	if (openStatus != BundleFileOpenStatus_CompressedDirectory)
		pInfo_Bundle->onDirectoryReady(*this);
	if (!mainWindow.OnFileEntryLoadSuccess(pTask.get(), pInfo, static_cast<TaskResult>(openStatus)))
		RemoveContextInfo(pInfo.get());
	return pInfo;
}
std::shared_ptr<FileContextInfo> Win32AppContext::OnFileOpenAsAssets(std::shared_ptr<FileOpenTask> pTask, AssetsFileContext* pContext,
	EAssetsFileOpenStatus openStatus, unsigned int parentFileID, unsigned int directoryEntryIdx)
{
	std::shared_ptr<FileContextInfo> pInfo = AppContext::OnFileOpenAsAssets(pTask, pContext, openStatus, parentFileID, directoryEntryIdx);
	if (pInfo == nullptr)
		return nullptr;
	AssetsFileContextInfo* pInfo_Assets = reinterpret_cast<AssetsFileContextInfo*>(pInfo.get());
	if (mainWindow.OnFileEntryLoadSuccess(pTask.get(), pInfo, static_cast<TaskResult>(openStatus)))
	{
		if (!pInfo_Assets->FindClassDatabase(classPackage))
			mainWindow.OnFindClassDatabaseFailure(pInfo_Assets, classPackage);
		pInfo_Assets->EnqueueContainersTask(*this, std::shared_ptr<AssetsFileContextInfo>(pInfo, pInfo_Assets));
	}
	else
		RemoveContextInfo(pInfo.get());
	return pInfo;
}
std::shared_ptr<FileContextInfo> Win32AppContext::OnFileOpenAsResources(std::shared_ptr<FileOpenTask> pTask, ResourcesFileContext* pContext,
	unsigned int parentFileID, unsigned int directoryEntryIdx)
{
	std::shared_ptr<FileContextInfo> pInfo = AppContext::OnFileOpenAsResources(pTask, pContext, parentFileID, directoryEntryIdx);
	if (pInfo == nullptr)
		return nullptr;
	if (!mainWindow.OnFileEntryLoadSuccess(pTask.get(), pInfo, static_cast<TaskResult>(0)))
		RemoveContextInfo(pInfo.get());
	return pInfo;
}
std::shared_ptr<FileContextInfo> Win32AppContext::OnFileOpenAsGeneric(std::shared_ptr<FileOpenTask> pTask, GenericFileContext* pContext, unsigned int parentFileID, unsigned int directoryEntryIdx)
{
	std::shared_ptr<FileContextInfo> pInfo = AppContext::OnFileOpenAsGeneric(pTask, pContext, parentFileID, directoryEntryIdx);
	if (pInfo == nullptr)
		return nullptr;
	if (!mainWindow.OnFileEntryLoadSuccess(pTask.get(), pInfo, static_cast<TaskResult>(0)))
		RemoveContextInfo(pInfo.get());
	return pInfo;
}
void Win32AppContext::OnFileOpenFail(std::shared_ptr<FileOpenTask> pTask, std::string& logText)
{
	mainWindow.OnFileEntryLoadFailure(pTask.get(), logText);
}
void Win32AppContext::OnDecompressBundle(BundleFileContextInfo::DecompressTask* pTask, TaskResult result)
{
	if (result >= 0)
		mainWindow.OnDecompressSuccess(pTask);
	else
		mainWindow.OnDecompressFailure(pTask);
}
void Win32AppContext::RemoveContextInfo(FileContextInfo* info)
{
	AppContext::RemoveContextInfo(info);
	mainWindow.OnRemoveContextInfo(info);
}

void Win32AppContext::LoadSettings()
{
	gcMinAge = 15;
#ifdef __X64
	gcMemoryLimit = 2 * 1024 * 1024 * 1024ULL; //2GiB default
#else
	gcMemoryLimit = 1 * 1024 * 1024 * 1024; //1GiB default
#endif
	autoDetectDependencies = true;
	//TODO: Load from settings file.
}


int Win32AppContext::Run(size_t argc, char** argv)
{
	LoadSettings();
	std::string loadErrorMessage;
	if (!this->LoadClassDatabasePackage(baseDir, loadErrorMessage))
	{
		if (loadErrorMessage.size() == 0)
			loadErrorMessage = "Unable to load the class database package file (classdata.tpk).";
		MessageBoxA(NULL, loadErrorMessage.c_str(), "UABE", 16);
	}
	taskManager.setMaxThreads(4);
	mcTreeList_Initialize();
	mcMditab_Initialize();

	INITCOMMONCONTROLSEX init;
	init.dwSize = sizeof(init);
	init.dwICC = ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_TAB_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS;
	InitCommonControlsEx(&init);

	mainWindow.Initialize();
	loadAllPlugins(*this, this->plugins, this->getBaseDir() + "./Plugins");

	bulk_RunBulk();

	int ret = mainWindow.HandleMessages();
	mcTreeList_Terminate();
	//Wait for all tasks to complete.
	taskManager.setMaxThreads(0);
	return ret;
}

INT_PTR CALLBACK bulk_DlgProc(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	TCHAR lpszPassword[16];
	WORD cchPassword;

	switch (message)
	{
	case WM_INITDIALOG:
		return TRUE;
	}
	return FALSE;

	UNREFERENCED_PARAMETER(lParam);
}

void Win32AppContext::bulk_RunBulk() {
	const size_t ntheargs = __argc - 1;
	auto theargs = __wargv + 1;

	bool bulkexport = false;
	bool bulkimport = false;
	std::string openlist = std::string("");
	std::string dlldir = std::string("");
	std::string exportdir = std::string("");
	std::string importdir = std::string("");
	std::string savedir = std::string("");
	std::string notifyfile = std::string("");

	for (size_t i = 0; i < ntheargs;) {
		bool islast = i == ntheargs - 1;

		const wchar_t* thearg = theargs[i];
		size_t strlen = wcslen(thearg);
		if (wcsncmp(thearg, L"bulkexport", strlen) == 0) {
			bulkexport = true;
		} else if (wcsncmp(thearg, L"bulkimport", strlen) == 0) {
			bulkimport = true;
		} else if (wcsncmp(thearg, L"--openlist", strlen) == 0) {
			if (islast) throw std::runtime_error("Invalid args");
			i++;
			thearg = theargs[i];
			openlist = std::filesystem::path(thearg).string();
		} else if (wcsncmp(thearg, L"--dlldir", strlen) == 0) {
			if (islast) throw std::runtime_error("Invalid args");
			i++;
			thearg = theargs[i];
			dlldir = std::filesystem::path(thearg).string();
		} else if (wcsncmp(thearg, L"--importdir", strlen) == 0) {
			if (islast) throw std::runtime_error("Invalid args");
			i++;
			thearg = theargs[i];
			importdir = std::filesystem::path(thearg).string();
		} else if (wcsncmp(thearg, L"--exportdir", strlen) == 0) {
			if (islast) throw std::runtime_error("Invalid args");
			i++;
			thearg = theargs[i];
			exportdir = std::filesystem::path(thearg).string();
		} else if (wcsncmp(thearg, L"--savedir", strlen) == 0) {
			if (islast) throw std::runtime_error("Invalid args");
			i++;
			thearg = theargs[i];
			savedir = std::filesystem::path(thearg).string();
		} else if (wcsncmp(thearg, L"--notifyfile", strlen) == 0) {
			if (islast) throw std::runtime_error("Invalid args");
			i++;
			thearg = theargs[i];
			notifyfile = std::filesystem::path(thearg).string();
		}

		i++;
	}

	if (bulkexport && bulkimport) throw std::runtime_error("Invalid args");

	if (bulkexport) {
		this->bulk_isBulk = true;

		if (openlist.empty() || dlldir.empty() || exportdir.empty()) {
			throw std::runtime_error("Invalid args");
		}

		std::vector<std::string> filepaths;
		std::ifstream is;
		is.open(openlist);
		std::string fileline;
		while (is >> fileline) {
			filepaths.push_back(fileline);
		}
		is.close();

		this->bulk_nfiles_total = filepaths.size();
		this->bulk_dlldir = dlldir;
		this->bulk_exportdir = exportdir;
		this->bulk_notifyfile = notifyfile;

		for (std::string filepath : filepaths) {
			mainWindow.bulk_OpenFile(filepath);
		}

		mainWindow.bulk_SelectAll();

		this->bulk_inited = true;
		if (GetActiveWindow() == getMainWindow().getWindow()) {
			if (!(this->bulk_dlgshown)) {
				this->bulk_dlgshown = true;
				while (true) {
					MessageBox(this->getMainWindow().getWindow(), TEXT("Running AutoUABE.\n\nSee the progress in bottom left."), TEXT("AutoUABE"), 0);
				}
			}
		}
	}

	if (bulkimport) {
		this->bulk_isBulk = true;
		this->bulk_isImport = true;

		if (openlist.empty() || dlldir.empty() || importdir.empty() || savedir.empty()) {
			throw std::runtime_error("Invalid args");
		}

		std::vector<std::string> filepaths;
		std::ifstream is;
		is.open(openlist);
		std::string fileline;
		while (is >> fileline) {
			filepaths.push_back(fileline);
		}
		is.close();

		this->bulk_nfiles_total = filepaths.size();
		this->bulk_dlldir = dlldir;
		this->bulk_importdir = importdir;
		this->bulk_savedir = savedir;
		this->bulk_notifyfile = notifyfile;

		for (std::string filepath : filepaths) {
			mainWindow.bulk_OpenFile(filepath);
		}

		mainWindow.bulk_SelectAll();

		this->bulk_inited = true;
		if (GetActiveWindow() == getMainWindow().getWindow()) {
			if (!(this->bulk_dlgshown)) {
				this->bulk_dlgshown = true;
				while (true) {
					MessageBox(this->getMainWindow().getWindow(), TEXT("Running AutoUABE.\n\nSee the progress in bottom left."), TEXT("AutoUABE"), 0);
				}
			}
		}
	}
}

void AppContext::bulk_doneExportTexture() {
	Win32AppContext* pContext = dynamic_cast<Win32AppContext*>(this);
	pContext->getMainWindow().bulk_ExportAllAssets();
}

void AppContext::bulk_saveAll() {
	Win32AppContext* pContext = dynamic_cast<Win32AppContext*>(this);
	pContext->getMainWindow().bulk_SaveAll();
}

bool Win32AppContext::ShowAssetBatchImportDialog(IAssetBatchImportDesc* pDesc, std::string _basePath)
{
	CBatchImportDialog dialog(mainWindow.getHInstance(), pDesc, nullptr, std::move(_basePath));
	if (bulk_isBulk) {
		dialog.bulk_doinitok();
		return true;
	}
	return dialog.ShowModal(mainWindow.getWindow());
}

bool Win32AppContext::ShowAssetBatchImportDialog(IAssetBatchImportDesc* pDesc, IWin32AssetBatchImportDesc* pDescWin32, std::string _basePath)
{
	CBatchImportDialog dialog(mainWindow.getHInstance(), pDesc, pDescWin32, std::move(_basePath));
	if (bulk_isBulk) {
		dialog.bulk_doinitok();
		return true;
	}
	return dialog.ShowModal(mainWindow.getWindow());
}

std::string Win32AppContext::QueryAssetExportLocation(const std::vector<struct AssetUtilDesc>& assets,
	const std::string& extension, const std::string& extensionFilter)
{
	if (assets.empty())
		return "";
	if (assets.size() > 1)
	{
		if (this->bulk_isBulk) {
			return this->bulk_exportdir;
		}

		WCHAR* folderPathW = nullptr;
		if (!ShowFolderSelectDialog(this->getMainWindow().getWindow(), &folderPathW, L"Select an output directory", UABE_FILEDIALOG_EXPIMPASSET_GUID))
			return "";
		auto pFolderPath8 = unique_WideToMultiByte(folderPathW);
		FreeCOMFilePathBuf(&folderPathW);

		return std::string(pFolderPath8.get());
	}
	else
	{
		const AssetUtilDesc& assetToExport = assets[0];
		if (assetToExport.asset.pathID == 0)
		{
			MessageBox(this->getMainWindow().getWindow(),
				TEXT("Unable to resolve the selected asset!"),
				TEXT("Asset Bundle Extractor"),
				MB_ICONERROR);
			return "";
		}
		std::unordered_map<std::string, size_t> _tmp;
		std::string exportPath = assetToExport.makeExportFilePath(_tmp, extension);
		auto pExportPathT = unique_MultiByteToTCHAR(exportPath.c_str());

		if (this->bulk_isBulk) {

			auto pFilePath8 = unique_WideToMultiByte(pExportPathT.get());

			auto filename = std::filesystem::path(std::string(pFilePath8.get())).filename();
			auto retfile = std::filesystem::path(this->bulk_exportdir) / filename;
			return retfile.string();
		}

		auto pExtensionFilterW = unique_MultiByteToWide(extensionFilter.c_str());
		WCHAR* filePathW = nullptr;
		if (FAILED(ShowFileSaveDialog(this->getMainWindow().getWindow(), &filePathW, pExtensionFilterW.get(), nullptr,
			pExportPathT.get(), TEXT("Export an asset"),
			UABE_FILEDIALOG_EXPIMPASSET_GUID)))
			return "";
		auto pFilePath8 = unique_WideToMultiByte(filePathW);
		FreeCOMFilePathBuf(&filePathW);

		return std::string(pFilePath8.get());
	}
}

std::vector<std::string> Win32AppContext::QueryAssetImportLocation(std::vector<AssetUtilDesc>& assets,
	std::string extension, std::string _extensionRegex, std::string extensionFilter)
{
	if (assets.empty())
		return {};
	CWin32GenericBatchImportDialogDesc importDesc(assets, std::move(_extensionRegex), extensionFilter);

	if (bulk_isBulk) {
		bool doImport = this->ShowAssetBatchImportDialog(&importDesc, &importDesc, bulk_importdir);
	}
	else if (importDesc.getElements().size() > 1)
	{
		WCHAR* folderPathW = nullptr;
		if (!ShowFolderSelectDialog(this->getMainWindow().getWindow(), &folderPathW, L"Select an input directory", UABE_FILEDIALOG_EXPIMPASSET_GUID))
			return {};
		auto pFolderPath8 = unique_WideToMultiByte(folderPathW);
		FreeCOMFilePathBuf(&folderPathW);
		bool doImport = this->ShowAssetBatchImportDialog(&importDesc, &importDesc, std::string(pFolderPath8.get()));
		if (!doImport)
			return {};
	}
	else
	{
		const AssetUtilDesc& assetToImport = importDesc.getElements().front();
		if (assetToImport.asset.pathID == 0)
		{
			MessageBox(this->getMainWindow().getWindow(),
				TEXT("Unable to resolve the selected asset!"),
				TEXT("Asset Bundle Extractor"),
				MB_ICONERROR);
			return {};
		}
		std::unordered_map<std::string, size_t> _tmp;
		std::string exportPath = assetToImport.makeExportFilePath(_tmp, extension);
		auto pExportPathT = unique_MultiByteToTCHAR(exportPath.c_str());

		auto pExtensionFilterW = unique_MultiByteToWide(extensionFilter.c_str());
		WCHAR* filePathW = nullptr;
		if (FAILED(ShowFileOpenDialog(this->getMainWindow().getWindow(), &filePathW, pExtensionFilterW.get(), nullptr,
			pExportPathT.get(), TEXT("Import an asset"),
			UABE_FILEDIALOG_EXPIMPASSET_GUID)))
			return {};
		auto pFilePath8 = unique_WideToMultiByte(filePathW);
		FreeCOMFilePathBuf(&filePathW);

		importDesc.importFilePaths[0].assign(pFilePath8.get());
	}
	std::vector<std::string> importFilePaths = importDesc.getImportFilePaths();
	assets = importDesc.clearAndGetElements();
	return importFilePaths;
}
