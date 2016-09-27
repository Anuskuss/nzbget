/*
 *  This file is part of nzbget. See <http://nzbget.net>.
 *
 *  Copyright (C) 2016 Andrey Prygunkov <hugbug@users.sourceforge.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "nzbget.h"
#include "Options.h"
#include "DiskState.h"
#include "Log.h"
#include "FileSystem.h"
#include "Rename.h"

#ifndef DISABLE_PARCHECK
void RenameController::PostParRenamer::PrintMessage(Message::EKind kind, const char* format, ...)
{
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, 1024, format, args);
	va_end(args);
	text[1024-1] = '\0';

	m_owner->m_postInfo->GetNzbInfo()->AddMessage(kind, text);
}
#endif


void RenameController::PostRarRenamer::PrintMessage(Message::EKind kind, const char* format, ...)
{
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, 1024, format, args);
	va_end(args);
	text[1024 - 1] = '\0';

	m_owner->m_postInfo->GetNzbInfo()->AddMessage(kind, text);
}


RenameController::RenameController()
{
	debug("Creating RenameController");

#ifndef DISABLE_PARCHECK
	m_parRenamer.m_owner = this;
#endif

	m_rarRenamer.m_owner = this;
}

void RenameController::StartJob(PostInfo* postInfo)
{
	RenameController* renameController = new RenameController();
	renameController->m_postInfo = postInfo;
	renameController->SetAutoDestroy(false);

	postInfo->SetPostThread(renameController);

	renameController->Start();
}

void RenameController::Run()
{
	BString<1024> nzbName;
	CString destDir;
	CString finalDir;
	{
		GuardedDownloadQueue guard = DownloadQueue::Guard();
		nzbName = m_postInfo->GetNzbInfo()->GetName();
		destDir = m_postInfo->GetNzbInfo()->GetDestDir();
		finalDir = m_postInfo->GetNzbInfo()->GetFinalDir();
	}

	BString<1024> infoName("rename for %s", *nzbName);
	SetInfoName(infoName);

	PrintMessage(Message::mkInfo, "Checking renamed files for %s", *nzbName);

	ExecRename(destDir, finalDir, nzbName);

	if (IsStopped())
	{
		PrintMessage(Message::mkWarning, "Renaming cancelled for %s", *nzbName);
	}
	else if (m_renamedCount > 0)
	{
		if (m_renamedUsingPar == 0)
		{
			PrintMessage(Message::mkInfo, "Successfully renamed %i archive file(s) for %s", m_renamedCount, *nzbName);
		}
		else if (m_renamedUsingPar < m_renamedCount)
		{
			PrintMessage(Message::mkInfo, "Successfully renamed %i archive file(s) and %i other file(s) for %s",
				m_renamedCount - m_renamedUsingPar, m_renamedUsingPar, *nzbName);
		}
		else
		{
			PrintMessage(Message::mkInfo, "Successfully renamed %i file(s) for %s", m_renamedCount, *nzbName);
		}
	}
	else
	{
		PrintMessage(Message::mkInfo, "No renamed files found for %s", *nzbName);
	}

	RenameCompleted();
}

void RenameController::AddMessage(Message::EKind kind, const char* text)
{
	m_postInfo->GetNzbInfo()->AddMessage(kind, text);
}

void RenameController::ExecRename(const char* destDir, const char* finalDir, const char* nzbName)
{
	m_postInfo->SetStageTime(Util::CurrentTime());
	m_postInfo->SetStage(PostInfo::ptRenaming);
	m_postInfo->SetWorking(true);

	if (g_Options->GetParRename())
	{
#ifndef DISABLE_PARCHECK
		m_parRenamer.SetDestDir(m_postInfo->GetNzbInfo()->GetUnpackStatus() == NzbInfo::usSuccess &&
			!Util::EmptyStr(finalDir) ? finalDir : destDir);
		m_parRenamer.SetInfoName(nzbName);
		m_parRenamer.SetDetectMissing(m_postInfo->GetNzbInfo()->GetUnpackStatus() == NzbInfo::usNone);
		m_parRenamer.Execute();
#endif
	}

	m_renamedUsingPar = m_renamedCount;

	if (g_Options->GetRarRename() && m_postInfo->GetNzbInfo()->GetUnpackStatus() == NzbInfo::usNone)
	{
		m_rarRenamer.SetDestDir(destDir);
		m_rarRenamer.SetInfoName(nzbName);
		m_rarRenamer.Execute();
	}
}

void RenameController::RenameCompleted()
{
	GuardedDownloadQueue downloadQueue = DownloadQueue::Guard();

	m_postInfo->GetNzbInfo()->SetRenameStatus(m_renamedCount > 0 ? NzbInfo::rsSuccess : NzbInfo::rsFailure);

#ifndef DISABLE_PARCHECK
	if (m_parRenamer.HasMissedFiles() && m_postInfo->GetNzbInfo()->GetParStatus() <= NzbInfo::psSkipped)
	{
		m_parRenamer.PrintMessage(Message::mkInfo, "Requesting par-check/repair for %s to restore missing files ", m_parRenamer.GetInfoName());
		m_postInfo->SetRequestParCheck(true);
	}
#endif

	m_postInfo->SetWorking(false);
	m_postInfo->SetStage(PostInfo::ptQueued);

	downloadQueue->Save();
}

#ifndef DISABLE_PARCHECK
void RenameController::UpdateParRenameProgress()
{
	GuardedDownloadQueue guard = DownloadQueue::Guard();

	m_postInfo->SetProgressLabel(m_parRenamer.GetProgressLabel());
	m_postInfo->SetStageProgress(m_parRenamer.GetStageProgress());
}
#endif

void RenameController::UpdateRarRenameProgress()
{
	GuardedDownloadQueue guard = DownloadQueue::Guard();

	m_postInfo->SetProgressLabel(m_rarRenamer.GetProgressLabel());
	m_postInfo->SetStageProgress(m_rarRenamer.GetStageProgress());
}

/**
*  Update file name in the CompletedFiles-list of NZBInfo
*/
void RenameController::RegisterRenamedFile(const char* oldFilename, const char* newFileName)
{
	for (CompletedFile& completedFile : m_postInfo->GetNzbInfo()->GetCompletedFiles())
	{
		if (!strcasecmp(completedFile.GetFileName(), oldFilename))
		{
			completedFile.SetFileName(newFileName);
			break;
		}
	}
	m_renamedCount++;
}
