#include "Project.h"

#include "data/access/StorageAccessProxy.h"
#include "data/graph/Token.h"
#include "data/parser/cxx/TaskParseWrapper.h"
#include "data/parser/java/TaskParseJava.h"
#include "data/PersistentStorage.h"
#include "data/TaskCleanStorage.h"

#include "settings/ApplicationSettings.h"
#include "settings/ProjectSettings.h"

#include "utility/file/FileRegister.h"
#include "utility/file/FileSystem.h"
#include "utility/logging/logging.h"
#include "utility/messaging/type/MessageFinishedParsing.h"
#include "utility/scheduling/TaskGroupSequential.h"
#include "utility/scheduling/TaskGroupParallel.h"
#include "utility/text/TextAccess.h"
#include "utility/utility.h"
#include "utility/utilityString.h"
#include "utility/Version.h"

#include "Application.h"
#include "CxxProject.h"
#include "JavaProject.h"
#include "isTrial.h"

std::shared_ptr<Project> Project::create(const FilePath& projectSettingsFile, StorageAccessProxy* storageAccessProxy)
{
	std::shared_ptr<Project> project;

	switch (ProjectSettings::getLanguageOfProject(projectSettingsFile))
	{
	case LANGUAGE_C:
	case LANGUAGE_CPP:
		{
			project = std::shared_ptr<CxxProject>(new CxxProject(
				std::make_shared<CxxProjectSettings>(projectSettingsFile), storageAccessProxy
			));
		}
		break;
	case LANGUAGE_JAVA:
		{
			project = std::shared_ptr<JavaProject>(new JavaProject(
				std::make_shared<JavaProjectSettings>(projectSettingsFile), storageAccessProxy
			));
		}
		break;
	}

	if (project)
	{
		project->load();
	}
	return project;
}

Project::~Project()
{
}

void Project::refresh()
{
	if (allowsRefresh())
	{
		bool loadedSettings = getProjectSettings()->reload();

		updateFileManager(m_fileManager);

		buildIndex();

		m_state = PROJECT_STATE_LOADED;
	}
}

void Project::forceRefresh()
{
	if (allowsRefresh())
	{
		clearStorage();
	}
	refresh();
}

FilePath Project::getProjectSettingsFilePath() const
{
	return getProjectSettings()->getFilePath();
}

std::string Project::getDescription() const
{
	return getProjectSettings()->getDescription();
}

bool Project::settingsEqualExceptNameAndLocation(const ProjectSettings& otherSettings) const
{
	return getProjectSettings()->equalsExceptNameAndLocation(otherSettings);
}

void Project::logStats() const
{
	m_storage->logStats();
}

Project::Project(StorageAccessProxy* storageAccessProxy)
	: m_storageAccessProxy(storageAccessProxy)
	, m_state(PROJECT_STATE_NOT_LOADED)
{
}

void Project::load()
{
	const std::shared_ptr<ProjectSettings> projectSettings = getProjectSettings();
	bool loadedSettings = projectSettings->reload();
	if (loadedSettings)
	{
		NameHierarchy::setDelimiter(getSymbolNameDelimiterForLanguage(projectSettings->getLanguage()));
		const FilePath projectSettingsPath = projectSettings->getFilePath();
		const FilePath dbPath = FilePath(projectSettingsPath).replaceExtension("coatidb");
		m_storage = std::make_shared<PersistentStorage>(dbPath);
		m_storageAccessProxy->setSubject(m_storage.get());

		if (m_storage->isEmpty())
		{
			m_state = PROJECT_STATE_EMPTY;
			m_storage->setup();
		}
		else if (m_storage->isIncompatible())
		{
			m_state = PROJECT_STATE_OUTVERSIONED;
		}
		else if (TextAccess::createFromFile(projectSettingsPath.str())->getText() != m_storage->getProjectSettingsText())
		{
			m_state = PROJECT_STATE_OUTDATED;
		}
		else
		{
			m_state = PROJECT_STATE_LOADED;
		}

		updateFileManager(m_fileManager);

		bool reparse = false;

		switch (m_state)
		{
		case PROJECT_STATE_EMPTY:
			buildIndex();
			m_state = PROJECT_STATE_LOADED;
			break;
		case PROJECT_STATE_OUTDATED:
			if (Application::getInstance()->hasGUI() && !isTrial())
			{
				std::vector<std::string> options;
				options.push_back("Yes");
				options.push_back("No");
				int result = Application::getInstance()->handleDialog(
					"The project file was changed after the last indexing. The project needs to get fully reindexed to "
					"reflect the current project state. Do you want to reindex the project?", options);

				reparse = (result == 0);
			}
			// dont break here.
		case PROJECT_STATE_LOADED:
			m_storage->finishParsing();
			MessageFinishedParsing(0, 0, 0, true).dispatch();
			break;
		case PROJECT_STATE_OUTVERSIONED:
			MessageStatus("Can't load project").dispatch();

			reparse = true;

			if (Application::getInstance()->hasGUI() && !isTrial())
			{
				std::vector<std::string> options;
				options.push_back("Yes");
				options.push_back("No");
				int result = Application::getInstance()->handleDialog(
					"This project was indexed with a different version of Coati. It needs to be fully reindexed to be used "
					"with this version of Coati. Do you want to reindex the project?", options);

				reparse = (result == 0);
			}
			m_storage.reset();
			break;
		}

		if (reparse)
		{
			forceRefresh();
		}
	}
}

void Project::clearStorage()
{
	if (!m_storage)
	{
		const FilePath projectSettingsPath = getProjectSettings()->getFilePath();
		const FilePath dbPath = FilePath(projectSettingsPath).replaceExtension("coatidb");
		m_storage = std::make_shared<PersistentStorage>(dbPath);
	}

	if (m_storage)
	{
		m_storage->clear();
		m_state = PROJECT_STATE_EMPTY;
	}
}

void Project::buildIndex()
{
	m_storage->setProjectSettingsText(TextAccess::createFromFile(getProjectSettingsFilePath().str())->getText());

	m_fileManager.fetchFilePaths(m_storage->getInfoOnAllFiles());
	std::set<FilePath> addedFilePaths = m_fileManager.getAddedFilePaths();
	std::set<FilePath> updatedFilePaths = m_fileManager.getUpdatedFilePaths();
	std::set<FilePath> removedFilePaths = m_fileManager.getRemovedFilePaths();

	utility::append(updatedFilePaths, m_storage->getDependingFilePaths(updatedFilePaths));
	utility::append(updatedFilePaths, m_storage->getDependingFilePaths(removedFilePaths));

	std::vector<FilePath> filesToClean;
	filesToClean.insert(filesToClean.end(), removedFilePaths.begin(), removedFilePaths.end());
	filesToClean.insert(filesToClean.end(), updatedFilePaths.begin(), updatedFilePaths.end());

	std::vector<FilePath> filesToParse;
	filesToParse.insert(filesToParse.end(), addedFilePaths.begin(), addedFilePaths.end());
	filesToParse.insert(filesToParse.end(), updatedFilePaths.begin(), updatedFilePaths.end());


	if (!filesToClean.size() && !filesToParse.size())
	{
		MessageFinishedParsing(0, 0, 0, true).dispatch();
		return;
	}

	std::shared_ptr<TaskGroupSequential> taskSequential = std::make_shared<TaskGroupSequential>();
	taskSequential->addTask(std::make_shared<TaskCleanStorage>(m_storage.get(), filesToClean));

	int indexerThreadCount = ApplicationSettings::getInstance()->getIndexerThreadCount();

	std::shared_ptr<FileRegister> fileRegister = std::make_shared<FileRegister>(&m_fileManager, indexerThreadCount > 1);
	fileRegister->setFilePaths(filesToParse);

	std::shared_ptr<TaskParseWrapper> taskParserWrapper = std::make_shared<TaskParseWrapper>(
		m_storage.get(),
		fileRegister
	);
	taskSequential->addTask(taskParserWrapper);

	std::shared_ptr<TaskGroupParallel> taskParallelIndexing = std::make_shared<TaskGroupParallel>();
	taskParserWrapper->setTask(taskParallelIndexing);

	std::shared_ptr<std::mutex> storageMutex = std::make_shared<std::mutex>();

	for (int i = 0; i < indexerThreadCount; i++)
	{
		taskParallelIndexing->addTask(createIndexerTask(m_storage.get(), storageMutex, fileRegister));
	}

	Task::dispatch(taskSequential);
}

bool Project::allowsRefresh()
{
	return true;
}


