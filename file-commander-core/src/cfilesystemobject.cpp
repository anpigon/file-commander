#include "cfilesystemobject.h"
#include "iconprovider/ciconprovider.h"
#include "filesystemhelperfunctions.h"
#include "windows/windowsutils.h"
#include "assert/advanced_assert.h"

#include "fasthash.h"

#ifdef CFILESYSTEMOBJECT_TEST
#define QFileInfo QFileInfo_Test
#define QDir QDir_Test
#endif

DISABLE_COMPILER_WARNINGS
#include <QDateTime>
#include <QDebug>
RESTORE_COMPILER_WARNINGS

#include <assert.h>
#include <errno.h>

#if defined __linux__ || defined __APPLE__ || defined __FreeBSD__
#include <unistd.h>
#include <sys/stat.h>
#include <wordexp.h>
#elif defined _WIN32
#include "windows/windowsutils.h"

#include <Windows.h>
#include <Shlwapi.h>
#pragma comment(lib, "Shlwapi.lib") // This lib would have to be added not just to the top level application, but every plugin as well, so using #pragma instead
#endif

static QString expandEnvironmentVariables(const QString& string)
{
#ifdef _WIN32
	if (!string.contains('%'))
		return string;

	WCHAR source[16384 + 1];
	WCHAR result[16384 + 1];

	static_assert (sizeof(WCHAR) == 2);
	const auto length = string.toWCharArray(source);
	source[length] = 0;
	if (const auto resultLength = ExpandEnvironmentStringsW(source, result, static_cast<DWORD>(std::size(result))); resultLength > 0)
		return toPosixSeparators(QString::fromWCharArray(result, resultLength - 1));
	else
		return string;
#else
	QString result = string;
	if (result.startsWith('~'))
		result.replace(0, 1, getenv("HOME"));

	if (result.contains('$'))
	{
		wordexp_t p;
		wordexp("$HOME/bin", &p, 0);
		const auto w = p.we_wordv;
		if (p.we_wordc > 0)
			result = w[0];

		wordfree(&p);
	}

	return result;
#endif
}


CFileSystemObject::CFileSystemObject(const QFileInfo& fileInfo) : _fileInfo(fileInfo)
{
	refreshInfo();
}

CFileSystemObject::CFileSystemObject(const QString& path) : _fileInfo(expandEnvironmentVariables(path))
{
	refreshInfo();
}

static QString parentForAbsolutePath(QString absolutePath)
{
	if (absolutePath.endsWith('/'))
		absolutePath.chop(1);

	const int lastSlash = absolutePath.lastIndexOf('/');
	if (lastSlash <= 0)
		return {};

	absolutePath.truncate(lastSlash + 1); // Keep the slash as it signifies a directory rather than a file.
	return absolutePath;
}

CFileSystemObject & CFileSystemObject::operator=(const QString & path)
{
	setPath(path);
	return *this;
}

void CFileSystemObject::refreshInfo()
{
	_properties.exists = _fileInfo.exists();
	_properties.fullPath = _fileInfo.absoluteFilePath();

	if (_fileInfo.isShortcut()) // This is Windows-specific, place under #ifdef?
	{
		_properties.exists = true;
		_properties.type = File;
	}
	else if (_fileInfo.isFile())
		_properties.type = File;
	else if (_fileInfo.isDir())
	{
		// Normalization - very important for hash calculation and equality checking
		// C:/1/ must be equal to C:/1
		if (!_properties.fullPath.endsWith('/'))
			_properties.fullPath.append('/');

		_properties.type = _fileInfo.isBundle() ? Bundle : Directory;
	}
	else if (_properties.exists)
	{
#ifdef _WIN32
		qInfo() << _properties.fullPath << " is neither a file nor a dir";
#endif
	}
	else if (_properties.fullPath.endsWith('/'))
		_properties.type = Directory;

	_properties.hash = fasthash64(_properties.fullPath.constData(), _properties.fullPath.size() * sizeof(QChar), 0);


	if (_properties.type == File)
	{
		_properties.extension = _fileInfo.suffix();
		_properties.completeBaseName = _fileInfo.completeBaseName();
	}
	else if (_properties.type == Directory)
	{
		_properties.completeBaseName = _fileInfo.baseName();
		const QString suffix = _fileInfo.completeSuffix();
		if (!suffix.isEmpty())
			_properties.completeBaseName = _properties.completeBaseName % '.' % suffix;

		// Ugly temporary bug fix for #141
		if (_properties.completeBaseName.isEmpty() && _properties.fullPath.endsWith('/'))
		{
			const QFileInfo tmpInfo = QFileInfo(_properties.fullPath.left(_properties.fullPath.length() - 1));
			_properties.completeBaseName = tmpInfo.baseName();
			const QString sfx = tmpInfo.completeSuffix();
			if (!sfx.isEmpty())
				_properties.completeBaseName = _properties.completeBaseName % '.' % sfx;
		}
	}
	else if (_properties.type == Bundle)
	{
		_properties.extension = _fileInfo.suffix();
		_properties.completeBaseName = _fileInfo.completeBaseName();
	}

	_properties.fullName = _properties.type == Directory ? _properties.completeBaseName : _fileInfo.fileName();
	_properties.isCdUp = _properties.fullName == QLatin1String("..");
	// QFileInfo::canonicalPath() / QFileInfo::absolutePath are undefined for non-files
	_properties.parentFolder = parentForAbsolutePath(_properties.fullPath);

	if (!_properties.exists)
		return;

	_properties.creationDate = (time_t) _fileInfo.birthTime().toTime_t();
	_properties.modificationDate = _fileInfo.lastModified().toTime_t();
	_properties.size = _properties.type == File ? _fileInfo.size() : 0;
}

void CFileSystemObject::setPath(const QString& path)
{
	if (path.isEmpty())
	{
		*this = CFileSystemObject();
		return;
	}

	_rootFileSystemId = std::numeric_limits<uint64_t>::max();

	_fileInfo.setFile(expandEnvironmentVariables(path));

	refreshInfo();
}

bool CFileSystemObject::operator==(const CFileSystemObject& other) const
{
	return hash() == other.hash();
}


// Information about this object
bool CFileSystemObject::isValid() const
{
	return hash() != 0;
}

bool CFileSystemObject::exists() const
{
	return _properties.exists;
}

const CFileSystemObjectProperties &CFileSystemObject::properties() const
{
	return _properties;
}

FileSystemObjectType CFileSystemObject::type() const
{
	return _properties.type;
}

bool CFileSystemObject::isFile() const
{
	return _properties.type == File;
}

bool CFileSystemObject::isDir() const
{
	return _properties.type == Directory || _properties.type == Bundle;
}

bool CFileSystemObject::isBundle() const
{
	return _properties.type == Bundle;
}

bool CFileSystemObject::isEmptyDir() const
{
	return isDir() && QDir(fullAbsolutePath()).entryList(QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).empty();
}

bool CFileSystemObject::isCdUp() const
{
	return _properties.isCdUp;
}

bool CFileSystemObject::isExecutable() const
{
	return _fileInfo.permission(QFile::ExeUser) || _fileInfo.permission(QFile::ExeOwner) || _fileInfo.permission(QFile::ExeGroup) || _fileInfo.permission(QFile::ExeOther);
}

bool CFileSystemObject::isReadable() const
{
	return _fileInfo.isReadable();
}

// Apparently, it will return false for non-existing files
bool CFileSystemObject::isWriteable() const
{
	return _fileInfo.isWritable();
}

bool CFileSystemObject::isHidden() const
{
	return _fileInfo.isHidden();
}

QString CFileSystemObject::fullAbsolutePath() const
{
	assert(_properties.type != Directory || _properties.fullPath.isEmpty() || _properties.fullPath.endsWith('/'));
	return _properties.fullPath;
}

QString CFileSystemObject::parentDirPath() const
{
	assert(_properties.parentFolder.isEmpty() || _properties.parentFolder.endsWith('/'));
	return _properties.parentFolder;
}

const QIcon& CFileSystemObject::icon() const
{
	return CIconProvider::iconForFilesystemObject(*this);
}

uint64_t CFileSystemObject::size() const
{
	return _properties.size;
}

qulonglong CFileSystemObject::hash() const
{
	return _properties.hash;
}

const QFileInfo &CFileSystemObject::qFileInfo() const
{
	return _fileInfo;
}

uint64_t CFileSystemObject::rootFileSystemId() const
{
	if (_rootFileSystemId == std::numeric_limits<uint64_t>::max())
	{
#ifdef _WIN32
		WCHAR drivePath[32768];
		const auto pathLength = _properties.fullPath.toWCharArray(drivePath);
		drivePath[pathLength] = 0;
		const auto driveNumber = PathGetDriveNumberW(drivePath);
		if (driveNumber != -1)
			_rootFileSystemId = static_cast<uint64_t>(driveNumber);
#else
		struct stat info;
		const int ret = stat(_properties.fullPath.toUtf8().constData(), &info);
		if (ret == 0 || errno == ENOENT)
			_rootFileSystemId = (uint64_t) info.st_dev;
		else
		{
			qInfo() << __FUNCTION__ << "Failed to query device ID for" << _properties.fullPath;
			qInfo() << strerror(errno);
		}
#endif
	}

	return _rootFileSystemId;
}

bool CFileSystemObject::isNetworkObject() const
{
#ifdef _WIN32
	return _properties.fullPath.startsWith(QLatin1String("//")) && !_properties.fullPath.startsWith(QLatin1String("//?/"));
#else
	return false;
#endif
}

bool CFileSystemObject::isSymLink() const
{
	// Apparently, QFileInfo has a bug that makes it erroneously report a non-link as a link (Qt 5.11)
	return _fileInfo.isSymLink() && !_fileInfo.symLinkTarget().isEmpty();
}

QString CFileSystemObject::symLinkTarget() const
{
	return _fileInfo.symLinkTarget();
}

bool CFileSystemObject::isMovableTo(const CFileSystemObject& dest) const
{
	if (!isValid() || !dest.isValid())
		return false;

	const auto fileSystemId = rootFileSystemId();
	const auto otherFileSystemId = dest.rootFileSystemId();

	return fileSystemId == otherFileSystemId && fileSystemId != std::numeric_limits<uint64_t>::max() && otherFileSystemId != std::numeric_limits<uint64_t>::max();
}

// A hack to store the size of a directory after it's calculated
void CFileSystemObject::setDirSize(uint64_t size)
{
	_properties.size = size;
}

// File name without suffix, or folder name. Same as QFileInfo::completeBaseName.
QString CFileSystemObject::name() const
{
	return _properties.completeBaseName;
}

// Filename + suffix for files, same as name() for folders
QString CFileSystemObject::fullName() const
{
	return _properties.fullName;
}

QString CFileSystemObject::extension() const
{
	return _properties.extension;
}

QString CFileSystemObject::sizeString() const
{
	return _properties.type == File ? fileSizeToString(_properties.size) : QString();
}

QString CFileSystemObject::modificationDateString() const
{
	QDateTime modificationDate;
	modificationDate.setTime_t((uint)_properties.modificationDate);
	modificationDate = modificationDate.toLocalTime();
	return modificationDate.toString(QLatin1String("dd.MM.yyyy hh:mm"));
}


// Return the list of consecutive full paths leading from the specified target to its root.
// E. g. C:/Users/user/Documents/ -> {C:/Users/user/Documents/, C:/Users/user/, C:/Users/, C:/}
std::vector<QString> pathHierarchy(const QString& path)
{
	assert_r(!path.contains('\\'));
	assert_r(!path.contains(QStringLiteral("//")) || !path.rightRef(path.length() - 2).contains(QStringLiteral("//")));

	if (path.isEmpty())
		return {};
	else if (path == '/')
		return { path };

	QString pathItem = path.endsWith('/') ? path.left(path.length() - 1) : path;
	std::vector<QString> result{ path };
	while ((pathItem = QFileInfo(pathItem).absolutePath()).length() < result.back().length())
	{
		if (pathItem.endsWith('/'))
			result.emplace_back(pathItem);
		else
			result.emplace_back(pathItem + '/');
	}

	return result;
}