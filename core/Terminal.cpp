//---------------------------------------------------------------------------
#include <vcl.h>
#pragma hdrstop

#include "Terminal.h"

#include <SysUtils.hpp>
#include <FileCtrl.hpp>

#include "Common.h"
#include "FileBuffer.h"
#include "FileSystems.h"
#include "Interface.h"
#include "RemoteFiles.h"
#include "ScpFileSystem.h"
#include "SftpFileSystem.h"
#include "TextsCore.h"
//---------------------------------------------------------------------------
#pragma package(smart_init)
//---------------------------------------------------------------------------
#define COMMAND_ERROR_ARI(MESSAGE, REPEAT) \
  { \
    int Result = CommandError(&E, MESSAGE, qaRetry | qaSkip | qaAbort); \
    switch (Result) { \
      case qaRetry: { REPEAT; } break; \
      case qaAbort: Abort(); \
    } \
  }
//---------------------------------------------------------------------------
__fastcall TTerminal::TTerminal(): TSecureShell()
{
  FFiles = new TRemoteDirectory(this);
  FExceptionOnFail = 0;
  FInTransaction = 0;
  FReadCurrentDirectoryPending = false;
  FReadDirectoryPending = false;
  FUserGroupsLookedup = False;
  FUserGroups = new TUserGroupsList();
  FOnProgress = NULL;
  FOnFinished = NULL;
  FUseBusyCursor = True;
  FLockDirectory = "";
  FDirectoryCache = new TRemoteDirectoryCache();
}
//---------------------------------------------------------------------------
__fastcall TTerminal::~TTerminal()
{
  SAFE_DESTROY(FFileSystem);
  delete FFiles;
  delete FUserGroups;
  delete FDirectoryCache;
}
//---------------------------------------------------------------------------
AnsiString __fastcall TTerminal::GetProtocolName()
{
  assert(FFileSystem);
  return FFileSystem->ProtocolName;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::Close()
{
  // file system cannot be destoryed here, moved to destructor
  TSecureShell::Close();
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::Open()
{
  TSecureShell::Open();
  assert(!FFileSystem);
  if ((SessionData->FSProtocol == fsSCPonly) ||
      (SessionData->FSProtocol == fsSFTP && SshFallbackCmd()) ||
      (!Configuration->ExpertMode))
  {
    FFileSystem = new TSCPFileSystem(this);
    LogEvent("Using SCP protocol");
  }
  else
  {
    FFileSystem = new TSFTPFileSystem(this);
    LogEvent("Using SFTP protocol");
  }
}
//---------------------------------------------------------------------------
bool __fastcall TTerminal::GetIsCapable(TFSCapability Capability) const
{
  assert(FFileSystem);
  return FFileSystem->IsCapable(Capability);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReactOnCommand(int /*TFSCommand*/ Cmd)
{
  bool ChangesDirectory = false;
  bool ModifiesFiles = false;

  switch ((TFSCommand)Cmd) {
    case fsChangeDirectory:
    case fsHomeDirectory:
      ChangesDirectory = true;
      break;

    case fsCopyToRemote:
    case fsDeleteFile:
    case fsRenameFile:
    case fsCreateDirectory:
    case fsChangeMode:
    case fsChangeGroup:
    case fsChangeOwner:
    case fsChangeProperties:
    case fsAnyCommand:
      ModifiesFiles = true;
      break;
  }

  if (ChangesDirectory)
  {
    if (!FInTransaction)
    {
      ReadCurrentDirectory();
      ReadDirectory(false);
    }
      else
    {
      FReadCurrentDirectoryPending = true;
      FReadDirectoryPending = true;
    }
  }
    else
  if (ModifiesFiles)
  {
    if (!FInTransaction) ReadDirectory(true);
      else FReadDirectoryPending = true;
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::TerminalError(AnsiString Msg)
{
  TerminalError(NULL, Msg);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::TerminalError(Exception * E, AnsiString Msg)
{
  throw ETerminal(E, Msg);
}
//---------------------------------------------------------------------------
int __fastcall TTerminal::FileOperationLoop(TFileOperationEvent CallBackFunc,
  TFileOperationProgressType * OperationProgress, bool AllowSkip,
  const AnsiString Message, void * Param1, void * Param2)
{
  assert(CallBackFunc);
  int Result;
  TTerminal * FTerminal = this; // required by FILE_OPERATION_LOOP_EX macro
  FILE_OPERATION_LOOP_EX
  (
    "", AllowSkip, Message,
    Result = CallBackFunc(Param1, Param2);
  );
  return Result;
}
//---------------------------------------------------------------------------
AnsiString __fastcall TTerminal::TranslateLockedPath(AnsiString Path, bool Lock)
{
  if (!SessionData->LockInHome || Path.IsEmpty() || (Path[1] != '/'))
    return Path;

  if (Lock)
  {
    if (Path.SubString(1, FLockDirectory.Length()) == FLockDirectory)
    {
      Path.Delete(1, FLockDirectory.Length());
      if (Path.IsEmpty()) Path = "/";
    }
  }
  else
  {
    Path = UnixExcludeTrailingBackslash(FLockDirectory + Path);
  }
  return Path;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::SetCurrentDirectory(AnsiString value)
{
  assert(FFileSystem);
  value = TranslateLockedPath(value, false);
  if (value != FFileSystem->CurrentDirectory)
  {
    ChangeDirectory(value);
  }
}
//---------------------------------------------------------------------------
AnsiString __fastcall TTerminal::GetCurrentDirectory()
{
  if (FFileSystem)
  {
    FCurrentDirectory = FFileSystem->CurrentDirectory;
    if (FCurrentDirectory.IsEmpty())
    {
      ReadCurrentDirectory();
    }
  }

  return TranslateLockedPath(FCurrentDirectory, true);
}
//---------------------------------------------------------------------------
TUserGroupsList * __fastcall TTerminal::GetUserGroups()
{
  assert(FFileSystem);
  if (!FUserGroupsLookedup && SessionData->LookupUserGroups &&
      IsCapable[fcUserGroupListing])
  {
    LookupUserGroups();
  }
  return FUserGroups;
}
//---------------------------------------------------------------------------
AnsiString __fastcall TTerminal::GetUserName() const
{
  // in future might be implemented to detect username similar to GetUserGroups
  return SessionData->UserName;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoChangeDirectory()
{
  if (FOnChangeDirectory) FOnChangeDirectory(this);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoReadDirectory(bool ReloadOnly)
{
  if (FOnReadDirectory) FOnReadDirectory(this, ReloadOnly);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::BeginTransaction()
{
  if (!FInTransaction)
  {
    FReadCurrentDirectoryPending = false;
    FReadDirectoryPending = false;
  }
  FInTransaction++;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::EndTransaction()
{
  // it connection was closed due to fatal error during transaction, do nothing
  if (Active)
  {
    if (!FInTransaction)
      TerminalError("Can't end transaction, not in transaction");
    assert(FInTransaction > 0);
    FInTransaction--;

    if (FInTransaction == 0)
    {
      try
      {
        if (FReadCurrentDirectoryPending) ReadCurrentDirectory();
        if (FReadDirectoryPending) ReadDirectory(!FReadCurrentDirectoryPending);
      }
      __finally
      {
        FReadCurrentDirectoryPending = false;
        FReadDirectoryPending = false;
      }
    }
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::SetExceptionOnFail(bool value)
{
  if (value) FExceptionOnFail++;
    else
  {
    if (FExceptionOnFail == 0)
      throw Exception("ExceptionOnFail is already zero.");
    FExceptionOnFail--;
  }
}
//---------------------------------------------------------------------------
bool __fastcall TTerminal::GetExceptionOnFail() const
{
  return (bool)(FExceptionOnFail > 0);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CommandError(Exception * E, const AnsiString Msg)
{
  CommandError(E, Msg, 0);
}
//---------------------------------------------------------------------------
int __fastcall TTerminal::CommandError(Exception * E, const AnsiString Msg,
  int Answers)
{
  int Result = 0;
  if (E && E->InheritsFrom(__classid(EFatal)))
  {
    FatalError(E, Msg);
  }
  else if (E && E->InheritsFrom(__classid(EAbort)))
  {
    // resent EAbort exception
    Abort();
  }
  else if (ExceptionOnFail)
  {
    throw ECommand(E, Msg);
  }
  else if (!Answers)
  {
    ECommand * ECmd = new ECommand(E, Msg);
    try
    {
      ShowExtendedException(ECmd, this);
    }
    __finally
    {
      delete ECmd;
    }
  }
  else
  {
    Result = DoQueryUser(Msg, E, Answers, 0);
  }
  return Result;
}
//---------------------------------------------------------------------------
bool __fastcall TTerminal::HandleException(Exception * E)
{
  if (ExceptionOnFail)
  {
    return false;
  }
  else
  {
    HandleExtendedException(E, this);
    return true;
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CloseOnCompletion()
{
  LogEvent("Closing session after completed operation (as requested by user)");
  Close();
  throw ESshTerminate(NULL, LoadStr(CLOSED_ON_COMPLETION));
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::FileModified(const TRemoteFile * File)
{
  assert(File);
  if (SessionData->CacheDirectories && File->Directory
      /*!!! && File->Directory == FFiles*/)
  {
    if (File->IsDirectory)
    {
      FDirectoryCache->ClearFileList(
        //!!!UnixIncludeTrailingBackslash(CurrentDirectory)+
        File->Directory->FullDirectory + File->FileName, true);
    }
    FDirectoryCache->ClearFileList(File->Directory->Directory, false);
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DirectoryModified(const AnsiString UnlockedPath, bool SubDirs)
{
  FDirectoryCache->ClearFileList(TranslateLockedPath(UnlockedPath, true),
    SubDirs);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReloadDirectory()
{
  if (SessionData->CacheDirectories)
  {
    FDirectoryCache->ClearFileList(CurrentDirectory, false);
  }

  ReadCurrentDirectory();
  FReadCurrentDirectoryPending = false;
  ReadDirectory(True);
  FReadDirectoryPending = false;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::EnsureNonExistence(const AnsiString FileName)
{
  // if filename doesn't contain path, we check for existence of file
  if ((UnixExtractFileDir(FileName) == FileName) &&
      UnixComparePaths(CurrentDirectory, FFiles->Directory))
  {
    TRemoteFile *File = FFiles->FindFile(FileName);
    if (File)
    {
      if (File->IsDirectory) throw ECommand(NULL, FMTLOAD(RENAME_CREATE_DIR_EXISTS, (FileName)));
        else throw ECommand(NULL, FMTLOAD(RENAME_CREATE_FILE_EXISTS, (FileName)));
    }
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoStartup()
{
  LogEvent("Doing startup conversation with host.");
  BeginTransaction();
  try
  {
    UpdateStatus(sshStartup);

    // Make sure that directory would be loaded at last
    FReadCurrentDirectoryPending = true;
    FReadDirectoryPending = true;

    FFileSystem->DoStartup();

    if (SessionData->LookupUserGroups && IsCapable[fcUserGroupListing])
    {
      LookupUserGroups();
    }

    UpdateStatus(sshOpenDirectory);
    if (!SessionData->RemoteDirectory.IsEmpty())
    {
      ChangeDirectory(SessionData->RemoteDirectory);
    }

  }
  __finally
  {
    EndTransaction();
  }
  LogEvent("Startup conversation with host finished.");
  UpdateStatus(sshReady);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReadCurrentDirectory()
{
  assert(FFileSystem);
  try
  {
    LogEvent("Getting current directory name.");
    AnsiString OldDirectory = FFileSystem->CurrentDirectory;

    FFileSystem->ReadCurrentDirectory();
    ReactOnCommand(fsCurrentDirectory);

    if (OldDirectory.IsEmpty())
    {
      FLockDirectory = (SessionData->LockInHome ?
        FFileSystem->CurrentDirectory : AnsiString(""));
    }
    if (OldDirectory != FFileSystem->CurrentDirectory) DoChangeDirectory();
  }
  catch (Exception &E)
  {
    CommandError(&E, LoadStr(READ_CURRENT_DIR_ERROR));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReadDirectory(bool ReloadOnly)
{
  TRemoteFileList * CachedFileList = NULL;
  if (SessionData->CacheDirectories)
  {
    CachedFileList = FDirectoryCache->GetFileList(CurrentDirectory);
  }

  if (CachedFileList)
  {
    CachedFileList->DuplicateTo(FFiles);
    DoReadDirectory(ReloadOnly);
    LogEvent("Directory content loaded from cache.");
  }
  else
  {
    FFiles->Directory = CurrentDirectory;

    try
    {
      try
      {
        CustomReadDirectory(FFiles);
      }
      __finally
      {
        if (Active)
        {
          // this must be called before error is displayed, otherwise
          // TUnixDirView would be drawn with invalid data (it keeps reference
          // to already destoroyed old listing)
          DoReadDirectory(ReloadOnly);
          if (SessionData->CacheDirectories)
          {
            FDirectoryCache->AddFileList(FFiles);
          }
        }
      }
    }
    catch (Exception &E)
    {
      CommandError(&E, FmtLoadStr(LIST_DIR_ERROR, ARRAYOFCONST((FFiles->Directory))));
    }
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CustomReadDirectory(TRemoteFileList * FileList)
{
  assert(FileList);
  assert(FFileSystem);
  FFileSystem->ReadDirectory(FileList);
  ReactOnCommand(fsListDirectory);
}
//---------------------------------------------------------------------------
TRemoteFileList * TTerminal::ReadDirectoryListing(AnsiString Directory)
{
  TRemoteFileList * FileList;
  try
  {
    FileList = new TRemoteFileList();
    try
    {
      FileList->Directory = Directory;
      AnsiString Directory = UnixIncludeTrailingBackslash(Directory);
      ExceptionOnFail = true;
      try
      {
        ReadDirectory(FileList);
      }
      __finally
      {
        ExceptionOnFail = false;
      }
    }
    catch(...)
    {
      delete FileList;
      throw;
    }
  }
  catch(Exception & E)
  {
    COMMAND_ERROR_ARI
    (
      "",
      FileList = ReadDirectoryListing(Directory);
    );
  }
  return FileList;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ProcessDirectory(const AnsiString DirName,
  TProcessFileEvent CallBackFunc, void * Param)
{
  TRemoteFileList * FileList = ReadDirectoryListing(DirName);

  try
  {
    AnsiString Directory = UnixIncludeTrailingBackslash(DirName);

    TRemoteFile * File;
    for (int Index = 0; Index < FileList->Count; Index++)
    {
      File = FileList->Files[Index];
      if (!File->IsParentDirectory && !File->IsThisDirectory)
      {
        CallBackFunc(Directory + File->FileName, File, Param);
      }
    }
  }
  __finally
  {
    delete FileList;
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReadDirectory(TRemoteFileList * FileList)
{
  try
  {
    CustomReadDirectory(FileList);
  }
  catch (Exception &E)
  {
    CommandError(&E, FmtLoadStr(LIST_DIR_ERROR, ARRAYOFCONST((FileList->Directory))));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReadSymlink(TRemoteFile * SymlinkFile,
  TRemoteFile *& File)
{
  assert(FFileSystem);
  try
  {
    LogEvent(FORMAT("Reading symlink \"%s\".", (SymlinkFile->FileName)));
    FFileSystem->ReadSymlink(SymlinkFile, File);
    ReactOnCommand(fsReadSymlink);
  }
  catch (Exception &E)
  {
    CommandError(&E, FMTLOAD(READ_SYMLINK_ERROR, (SymlinkFile->FileName)));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ReadFile(const AnsiString FileName,
  TRemoteFile *& File)
{
  assert(FFileSystem);
  File = NULL;
  try
  {
    LogEvent(FORMAT("Listing file \"%s\".", (FileName)));
    FFileSystem->ReadFile(FileName, File);
    ReactOnCommand(fsListFile);
  }
  catch (Exception &E)
  {
    if (File) delete File;
    File = NULL;
    CommandError(&E, FMTLOAD(LIST_DIR_ERROR, (FileName)));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ProcessFiles(TStrings * FileList,
  TFileOperation Operation, TProcessFileEvent ProcessFile, void * Param)
{
  assert(FFileSystem);
  assert(FileList);

  bool DisconnectWhenComplete = false;

  try
  {
    TFileOperationProgressType Progress(FOnProgress, FOnFinished);
    Progress.Start(Operation, osRemote, FileList->Count);

    FOperationProgress = &Progress;
    try
    {
      BeginTransaction();
      try
      {
        int Index = 0;
        AnsiString FileName;
        bool Success;
        while ((Index < FileList->Count) && (Progress.Cancel == csContinue))
        {
          FileName = FileList->Strings[Index];
          try
          {
            Success = false;
            ProcessFile(FileName, (TRemoteFile *)FileList->Objects[Index], Param);
            Success = true;
          }
          __finally
          {
            Progress.Finish(UnixExtractFileName(FileName),
              Success, DisconnectWhenComplete);
          }
          Index++;
        }
      }
      __finally
      {
        EndTransaction();
      }
    }
    __finally
    {
      FOperationProgress = NULL;
      Progress.Stop();
    }
  }
  catch (Exception &E)
  {
    DisconnectWhenComplete = false;
  }

  if (DisconnectWhenComplete)
  {
    CloseOnCompletion();
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DeleteFile(AnsiString FileName,
  const TRemoteFile * File, void * Recursive)
{
  if (FileName.IsEmpty() && File)
  {
    FileName = File->FileName;
  }
  if (OperationProgress && OperationProgress->Operation == foDelete)
  {
    if (OperationProgress->Cancel != csContinue) Abort();
    OperationProgress->SetFile(FileName);
  }
  LogEvent(FORMAT("Deleting file \"%s\".", (FileName)));
  if (File) FileModified(File);
  DoDeleteFile(FileName, File, Recursive);
  ReactOnCommand(fsDeleteFile);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoDeleteFile(const AnsiString FileName,
  const TRemoteFile * File, void * Recursive)
{
  try
  {
    assert(FFileSystem);
    // 'File' parameter: SFTPFileSystem needs to know if file is file or directory
    FFileSystem->DeleteFile(FileName, File, 
      Recursive ? *((bool*)Recursive) : true);
  }
  catch(Exception & E)
  {
    COMMAND_ERROR_ARI
    (
      FMTLOAD(DELETE_FILE_ERROR, (FileName)),
      DoDeleteFile(FileName, File, Recursive)
    );
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DeleteFiles(TStrings * FilesToDelete, bool * Recursive)
{
  ProcessFiles(FilesToDelete, foDelete, DeleteFile, Recursive);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ChangeFileProperties(AnsiString FileName,
  const TRemoteFile * File, /*const TRemoteProperties*/ void * Properties)
{
  TRemoteProperties * RProperties = (TRemoteProperties *)Properties;
  assert(RProperties && !RProperties->Valid.Empty());

  if (FileName.IsEmpty() && File)
  {
    FileName = File->FileName;
  }
  if (OperationProgress && OperationProgress->Operation == foSetProperties)
  {
    if (OperationProgress->Cancel != csContinue) Abort();
    OperationProgress->SetFile(FileName);
  }
  if (IsLogging())
  {
    LogEvent(FORMAT("Changing properties of \"%s\" (%s)",
      (FileName, BooleanToEngStr(RProperties->Recursive))));
    if (RProperties->Valid.Contains(vpRights))
    {
      LogEvent(FORMAT(" - mode: \"%s\"", (RProperties->Rights.ModeStr)));
    }
    if (RProperties->Valid.Contains(vpGroup))
    {
      LogEvent(FORMAT(" - group: \"%s\"", (RProperties->Group)));
    }
    if (RProperties->Valid.Contains(vpOwner))
    {
      LogEvent(FORMAT(" - owner: \"%s\"", (RProperties->Owner)));
    }
  }
  if (File) FileModified(File);
  DoChangeFileProperties(FileName, File, RProperties);
  ReactOnCommand(fsChangeProperties);
}
//---------------------------------------------------------------------------
/*void __fastcall TTerminal::ChangeFileProperties(const TRemoteFile * File,
  const TRemoteProperties * Properties)
{
  assert(File);
  FileModified(File);
  ChangeFileProperties(File->FileName, Properties, File);
} */
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoChangeFileProperties(const AnsiString FileName,
  const TRemoteFile * File, const TRemoteProperties * Properties)
{
  try
  {
    assert(FFileSystem);
    FFileSystem->ChangeFileProperties(FileName, File, Properties);
  }
  catch(Exception & E)
  {
    COMMAND_ERROR_ARI
    (
      FMTLOAD(CHANGE_PROPERTIES_ERROR, (FileName)),
      DoChangeFileProperties(FileName, File, Properties)
    );
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ChangeFilesProperties(TStrings * FileList,
  const TRemoteProperties * Properties)
{
  ProcessFiles(FileList, foSetProperties, ChangeFileProperties, (void *)Properties);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::RenameFile(const AnsiString FileName,
  const AnsiString NewName)
{
  LogEvent(FORMAT("Renaming file \"%s\" to \"%s\".", (FileName, NewName)));
  DoRenameFile(FileName, NewName);
  ReactOnCommand(fsRenameFile);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::RenameFile(const TRemoteFile * File,
  const AnsiString NewName, bool CheckExistence)
{
  assert(File && File->Directory == FFiles);
  bool Proceed = true;
  // if filename doesn't contain path, we check for existence of file
  if ((File->FileName != NewName) && CheckExistence &&
      Configuration->ConfirmOverwriting &&
      UnixComparePaths(CurrentDirectory, FFiles->Directory))
  {
    TRemoteFile * DuplicateFile = FFiles->FindFile(NewName);
    if (DuplicateFile)
    {
      AnsiString QuestionFmt;
      if (DuplicateFile->IsDirectory) QuestionFmt = LoadStr(DIRECTORY_OVERWRITE);
        else QuestionFmt = LoadStr(FILE_OVERWRITE);
      int Result;
      Result = DoQueryUser(FORMAT(QuestionFmt, (NewName)),
        qaYes | qaNo, qpNeverAskAgainCheck);
      if (Result == qaNeverAskAgain)
      {
        Proceed = true;
        Configuration->ConfirmOverwriting = false;
      }
        else
      {
        Proceed = (Result == qaYes);
      }
    }
  }

  if (Proceed)
  {
    FileModified(File);
    RenameFile(File->FileName, NewName);
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoRenameFile(const AnsiString FileName,
  const AnsiString NewName)
{
  try
  {
    assert(FFileSystem);
    FFileSystem->RenameFile(FileName, NewName);
  }
  catch(Exception & E)
  {
    COMMAND_ERROR_ARI
    (
      FMTLOAD(RENAME_FILE_ERROR, (FileName, NewName)),
      DoRenameFile(FileName, NewName)
    );
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CreateDirectory(const AnsiString DirName,
  const TRemoteProperties * Properties)
{
  assert(FFileSystem);
  EnsureNonExistence(DirName);
  if (SessionData->CacheDirectories)
  {
    FDirectoryCache->ClearFileList(CurrentDirectory, false);
  }

  LogEvent(FORMAT("Creating directory \"%s\".", (DirName)));
  DoCreateDirectory(DirName, Properties);
  ReactOnCommand(fsCreateDirectory);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoCreateDirectory(const AnsiString DirName,
  const TRemoteProperties * Properties)
{
  try
  {
    assert(FFileSystem);
    FFileSystem->CreateDirectory(DirName, Properties);
  }
  catch(Exception & E)
  {
    COMMAND_ERROR_ARI
    (
      FMTLOAD(CREATE_DIR_ERROR, (DirName)),
      DoCreateDirectory(DirName, Properties)
    );
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CreateLink(const AnsiString FileName,
  const AnsiString PointTo, bool Symbolic)
{
  assert(FFileSystem);
  EnsureNonExistence(FileName);
  if (SessionData->CacheDirectories)
  {
    FDirectoryCache->ClearFileList(CurrentDirectory, false);
  }

  LogEvent(FORMAT("Creating link \"%s\" to \"%s\" (symbolic: %s).",
    (FileName, PointTo, BooleanToEngStr(Symbolic))));
  DoCreateLink(FileName, PointTo, Symbolic);
  ReactOnCommand(fsCreateDirectory);
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::DoCreateLink(const AnsiString FileName,
  const AnsiString PointTo, bool Symbolic)
{
  try
  {
    assert(FFileSystem);
    FFileSystem->CreateLink(FileName, PointTo, Symbolic);
  }
  catch(Exception & E)
  {
    COMMAND_ERROR_ARI
    (
      FMTLOAD(CREATE_LINK_ERROR, (FileName)),
      DoCreateLink(FileName, PointTo, Symbolic);
    );
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::HomeDirectory()
{
  assert(FFileSystem);
  try
  {
    LogEvent("Changing directory to home directory.");
    FFileSystem->HomeDirectory();
    ReactOnCommand(fsHomeDirectory);
  }
  catch (Exception &E)
  {
    CommandError(&E, LoadStr(CHANGE_HOMEDIR_ERROR));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::ChangeDirectory(const AnsiString Directory)
{
  assert(FFileSystem);
  try
  {
    LogEvent(FORMAT("Changing directory to \"%s\".", (Directory)));
    FFileSystem->ChangeDirectory(Directory);
    ReactOnCommand(fsChangeDirectory);
  }
  catch (Exception &E)
  {
    CommandError(&E, FMTLOAD(CHANGE_DIR_ERROR, (Directory)));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::LookupUserGroups()
{
  assert(FFileSystem);
  assert(IsCapable[fcUserGroupListing]);

  try
  {
    FUserGroupsLookedup = true;
    LogEvent("Looking up current user groups.");
    FFileSystem->LookupUserGroups();
    ReactOnCommand(fsLookupUserGroups);

    if (IsLogging())
    {
      LogEvent("Following groups found:");
      for (int Index = 0; Index < FUserGroups->Count; Index++)
      {
        LogEvent(AnsiString("  ") + FUserGroups->Strings[Index]);
      }
    }
  }
  catch (Exception &E)
  {
    CommandError(&E, LoadStr(LOOKUP_GROUPS_ERROR));
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::AnyCommand(const AnsiString Command)
{
  assert(FFileSystem);
  try
  {
    LogEvent("Executing used defined command.");
    FFileSystem->AnyCommand(Command);
    ReactOnCommand(fsAnyCommand);
  }
  catch (Exception &E)
  {
    if (ExceptionOnFail || (E.InheritsFrom(__classid(EFatal)))) throw;
      else ShowExtendedException(&E, this);
  }
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::OpenLocalFile(const AnsiString FileName,
  int Access, int * AAttrs, HANDLE * AHandle, unsigned long * ACTime,
  unsigned long * AMTime, unsigned long * AATime, __int64 * ASize)
{
  TTerminal * FTerminal = this; //required by FILE_OPERATION_LOOP macro
  int Attrs = 0;
  HANDLE Handle = 0;

  FILE_OPERATION_LOOP (FileName, FMTLOAD(FILE_NOT_EXISTS, (FileName)),
    Attrs = FileGetAttr(FileName);
    if (Attrs == -1) EXCEPTION;
  )

  if ((Attrs & faDirectory) == 0)
  {
    FILE_OPERATION_LOOP (FileName, FMTLOAD(OPENFILE_ERROR, (FileName)),
      Handle = CreateFile(FileName.c_str(), Access,
        Access == GENERIC_READ ? FILE_SHARE_READ : 0,
        NULL, OPEN_EXISTING, 0, 0);
      if (Handle == INVALID_HANDLE_VALUE)
      {
        Handle = 0;
        EXCEPTION;
      }
    );

    try
    {
      if (AATime || AMTime)
      {
        // Get last file access and modification time
        FILE_OPERATION_LOOP (FileName, FMTLOAD(CANT_GET_ATTRS, (FileName)),
          FILETIME ATime;
          FILETIME MTime;
          FILETIME CTime;
          if (!GetFileTime(Handle, &CTime, &ATime, &MTime)) EXCEPTION;
          if (ACTime) TIME_WIN_TO_POSIX(CTime, *ACTime);
          if (AATime) TIME_WIN_TO_POSIX(ATime, *AATime);
          if (AMTime) TIME_WIN_TO_POSIX(MTime, *AMTime);
        );
      }

      if (ASize)
      {
        // Get file size
        FILE_OPERATION_LOOP (FileName, FMTLOAD(CANT_GET_ATTRS, (FileName)),
          unsigned long LSize;
          unsigned long HSize;
          LSize = GetFileSize(Handle, &HSize);
          if ((LSize == 0xFFFFFFFF) && (GetLastError() != NO_ERROR)) EXCEPTION;
          *ASize = (__int64(HSize) << 32) + LSize;
        );
      }
    }
    catch(...)
    {
      CloseHandle(Handle);
      throw;
    }
  }

  if (AAttrs) *AAttrs = Attrs;
  if (AHandle) *AHandle = Handle;
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CopyToRemote(TStrings * FilesToCopy,
  const AnsiString TargetDir, const TCopyParamType * CopyParam, int Params)
{
  assert(FFileSystem);
  assert(FilesToCopy);

  bool DisconnectWhenComplete = false;

  try
  {
    TFileOperationProgressType OperationProgress(FOnProgress, FOnFinished);
    OperationProgress.Start((Params & cpDelete ? foMove : foCopy), osLocal,
      FilesToCopy->Count, Params & cpDragDrop, TargetDir);

    FOperationProgress = &OperationProgress;
    try
    {
      AnsiString UnlockedTargetDir = TranslateLockedPath(TargetDir, false);
      BeginTransaction();
      try
      {
        if (IsLogging())
        {
          LogEvent(FORMAT("Copying %d files/directories to remote directory "
            "\"%s\"", (FilesToCopy->Count, TargetDir)));
          LogEvent(CopyParam->LogStr);
        }

        FFileSystem->CopyToRemote(FilesToCopy, UnlockedTargetDir,
          CopyParam, Params, &OperationProgress, DisconnectWhenComplete);
      }
      __finally
      {
        if (Active)
        {
          ReactOnCommand(fsCopyToRemote);
          EndTransaction();
        }
      }
    }
    __finally
    {
      OperationProgress.Stop();
      FOperationProgress = NULL;
    }
  }
  catch (Exception &E)
  {
    CommandError(&E, LoadStr(TOREMOTE_COPY_ERROR));
    DisconnectWhenComplete = false;
  }

  if (DisconnectWhenComplete) CloseOnCompletion();
}
//---------------------------------------------------------------------------
void __fastcall TTerminal::CopyToLocal(TStrings * FilesToCopy,
  const AnsiString TargetDir, const TCopyParamType * CopyParam, int Params)
{
  assert(FFileSystem);

  // see scp.c: sink(), tolocal()

  bool OwnsFileList = (FilesToCopy == NULL);
  bool DisconnectWhenComplete = false;

  try
  {
    if (OwnsFileList)
    {
      FilesToCopy = new TStringList();
      FilesToCopy->Assign(Files->SelectedFiles);
    }

    BeginTransaction();
    try
    {
      TFileOperationProgressType OperationProgress(FOnProgress, FOnFinished);
      OperationProgress.Start((Params & cpDelete ? foMove : foCopy), osRemote,
        FilesToCopy->Count, Params & cpDragDrop, TargetDir);

      FOperationProgress = &OperationProgress;
      try
      {

        try
        {
          try
          {
            FFileSystem->CopyToLocal(FilesToCopy, TargetDir, CopyParam, Params,
              &OperationProgress, DisconnectWhenComplete);
          }
          __finally
          {
            if (Active)
            {
              ReactOnCommand(fsCopyToLocal);
            }
          }
        }
        catch (Exception &E)
        {
          CommandError(&E, LoadStr(TOLOCAL_COPY_ERROR));
          DisconnectWhenComplete = false;
        }

      }
      __finally
      {
        FOperationProgress = NULL;
        OperationProgress.Stop();
      }

    }
    __finally
    {
      // If session is still active (no fatal error) we reload directory
      // by calling EndTransaction
      EndTransaction();
    }

  }
  __finally
  {
    if (OwnsFileList) delete FilesToCopy;
  }

  if (DisconnectWhenComplete) CloseOnCompletion();
}



