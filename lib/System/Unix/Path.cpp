//===- llvm/System/Unix/Path.cpp - Unix Path Implementation -----*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the Unix specific portion of the Path class.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only generic UNIX code that
//===          is guaranteed to work on *all* UNIX variants.
//===----------------------------------------------------------------------===//

#include <llvm/Config/config.h>
#include <llvm/Config/alloca.h>
#include "Unix.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <utime.h>
#include <dirent.h>

namespace llvm {
using namespace sys;

Path::Path(const std::string& unverified_path) 
  : path(unverified_path)
{
  if (unverified_path.empty())
    return;
  if (this->isValid()) 
    return;
  // oops, not valid.
  path.clear();
  ThrowErrno(unverified_path + ": path is not valid");
}

Path
Path::GetRootDirectory() {
  Path result;
  result.setDirectory("/");
  return result;
}

static void getPathList(const char*path, std::vector<sys::Path>& Paths) {
  const char* at = path;
  const char* delim = strchr(at, ':');
  Path tmpPath;
  while( delim != 0 ) {
    std::string tmp(at, size_t(delim-at));
    if (tmpPath.setDirectory(tmp))
      if (tmpPath.readable())
        Paths.push_back(tmpPath);
    at = delim + 1;
    delim = strchr(at, ':');
  }
  if (*at != 0)
    if (tmpPath.setDirectory(std::string(at)))
      if (tmpPath.readable())
        Paths.push_back(tmpPath);

}
void 
Path::GetSystemLibraryPaths(std::vector<sys::Path>& Paths) {
#ifdef LTDL_SHLIBPATH_VAR
  char* env_var = getenv(LTDL_SHLIBPATH_VAR);
  if (env_var != 0) {
    getPathList(env_var,Paths);
  }
#endif
  // FIXME: Should this look at LD_LIBRARY_PATH too?
  Paths.push_back(sys::Path("/usr/local/lib/"));
  Paths.push_back(sys::Path("/usr/X11R6/lib/"));
  Paths.push_back(sys::Path("/usr/lib/"));
  Paths.push_back(sys::Path("/lib/"));
}

void
Path::GetBytecodeLibraryPaths(std::vector<sys::Path>& Paths) {
  char * env_var = getenv("LLVM_LIB_SEARCH_PATH");
  if (env_var != 0) {
    getPathList(env_var,Paths);
  }
#ifdef LLVMGCCDIR
  {
    Path tmpPath(std::string(LLVMGCCDIR) + "bytecode-libs/");
    if (tmpPath.readable())
      Paths.push_back(tmpPath);
  }
#endif
#ifdef LLVM_LIBDIR
  {
    Path tmpPath;
    if (tmpPath.setDirectory(LLVM_LIBDIR))
      if (tmpPath.readable())
        Paths.push_back(tmpPath);
  }
#endif
  GetSystemLibraryPaths(Paths);
}

Path 
Path::GetLLVMDefaultConfigDir() {
  return Path("/etc/llvm/");
}

Path 
Path::GetLLVMConfigDir() {
  Path result;
  if (result.setDirectory(LLVM_ETCDIR))
    return result;
  return GetLLVMDefaultConfigDir();
}

Path
Path::GetUserHomeDirectory() {
  const char* home = getenv("HOME");
  if (home) {
    Path result;
    if (result.setDirectory(home))
      return result;
  }
  return GetRootDirectory();
}

bool
Path::isFile() const {
  return (isValid() && path[path.length()-1] != '/');
}

bool
Path::isDirectory() const {
  return (isValid() && path[path.length()-1] == '/');
}

std::string
Path::getBasename() const {
  // Find the last slash
  size_t slash = path.rfind('/');
  if (slash == std::string::npos)
    slash = 0;
  else
    slash++;

  return path.substr(slash, path.rfind('.'));
}

bool Path::hasMagicNumber(const std::string &Magic) const {
  size_t len = Magic.size();
  assert(len < 1024 && "Request for magic string too long");
  char* buf = (char*) alloca(1 + len);
  int fd = ::open(path.c_str(),O_RDONLY);
  if (fd < 0)
    return false;
  size_t read_len = ::read(fd, buf, len);
  close(fd);
  if (len != read_len)
    return false;
  buf[len] = '\0';
  return Magic == buf;
}

bool Path::getMagicNumber(std::string& Magic, unsigned len) const {
  if (!isFile())
    return false;
  assert(len < 1024 && "Request for magic string too long");
  char* buf = (char*) alloca(1 + len);
  int fd = ::open(path.c_str(),O_RDONLY);
  if (fd < 0)
    return false;
  ssize_t bytes_read = ::read(fd, buf, len);
  ::close(fd);
  if (ssize_t(len) != bytes_read) {
    Magic.clear();
    return false;
  }
  Magic.assign(buf,len);
  return true;
}

bool 
Path::isBytecodeFile() const {
  char buffer[ 4];
  buffer[0] = 0;
  int fd = ::open(path.c_str(),O_RDONLY);
  if (fd < 0)
    return false;
  ssize_t bytes_read = ::read(fd, buffer, 4);
  ::close(fd);
  if (4 != bytes_read) 
    return false;

  return (buffer[0] == 'l' && buffer[1] == 'l' && buffer[2] == 'v' &&
      (buffer[3] == 'c' || buffer[3] == 'm'));
}

bool
Path::exists() const {
  return 0 == access(path.c_str(), F_OK );
}

bool
Path::readable() const {
  return 0 == access(path.c_str(), F_OK | R_OK );
}

bool
Path::writable() const {
  return 0 == access(path.c_str(), F_OK | W_OK );
}

bool
Path::executable() const {
  return 0 == access(path.c_str(), R_OK | X_OK );
}

std::string 
Path::getLast() const {
  // Find the last slash
  size_t pos = path.rfind('/');

  // Handle the corner cases
  if (pos == std::string::npos)
    return path;

  // If the last character is a slash
  if (pos == path.length()-1) {
    // Find the second to last slash
    size_t pos2 = path.rfind('/', pos-1);
    if (pos2 == std::string::npos)
      return path.substr(0,pos);
    else
      return path.substr(pos2+1,pos-pos2-1);
  }
  // Return everything after the last slash
  return path.substr(pos+1);
}

void
Path::getStatusInfo(StatusInfo& info) const {
  struct stat buf;
  if (0 != stat(path.c_str(), &buf)) {
    ThrowErrno(std::string("Can't get status: ")+path);
  }
  info.fileSize = buf.st_size;
  info.modTime.fromEpochTime(buf.st_mtime);
  info.mode = buf.st_mode;
  info.user = buf.st_uid;
  info.group = buf.st_gid;
  info.isDir = S_ISDIR(buf.st_mode);
  if (info.isDir && path[path.length()-1] != '/')
    path += '/';
}

bool
Path::getDirectoryContents(std::set<Path>& result) const {
  if (!isDirectory())
    return false;
  DIR* direntries = ::opendir(path.c_str());
  if (direntries == 0)
    ThrowErrno(path + ": can't open directory");

  result.clear();
  struct dirent* de = ::readdir(direntries);
  while (de != 0) {
    if (de->d_name[0] != '.') {
      Path aPath(path + (const char*)de->d_name);
      struct stat buf;
      if (0 != stat(aPath.path.c_str(), &buf))
        ThrowErrno(aPath.path + ": can't get status");
      if (S_ISDIR(buf.st_mode))
        aPath.path += "/";
      result.insert(aPath);
    }
    de = ::readdir(direntries);
  }
  
  closedir(direntries);
  return true;
}

bool
Path::setDirectory(const std::string& a_path) {
  if (a_path.size() == 0)
    return false;
  Path save(*this);
  path = a_path;
  size_t last = a_path.size() -1;
  if (a_path.size() == 0 || a_path[last] != '/')
    path += '/';
  if (!isValid()) {
    path = save.path;
    return false;
  }
  return true;
}

bool
Path::setFile(const std::string& a_path) {
  if (a_path.size() == 0)
    return false;
  Path save(*this);
  path = a_path;
  size_t last = a_path.size() - 1;
  while (last > 0 && a_path[last] == '/')
    last--;
  path.erase(last+1);
  if (!isValid()) {
    path = save.path;
    return false;
  }
  return true;
}

bool
Path::appendDirectory(const std::string& dir) {
  if (isFile()) 
    return false;
  Path save(*this);
  path += dir;
  path += "/";
  if (!isValid()) {
    path = save.path;
    return false;
  }
  return true;
}

bool
Path::elideDirectory() {
  if (isFile()) 
    return false;
  size_t slashpos = path.rfind('/',path.size());
  if (slashpos == 0 || slashpos == std::string::npos)
    return false;
  if (slashpos == path.size() - 1)
    slashpos = path.rfind('/',slashpos-1);
  if (slashpos == std::string::npos)
    return false;
  path.erase(slashpos);
  return true;
}

bool
Path::appendFile(const std::string& file) {
  if (!isDirectory()) 
    return false;
  Path save(*this);
  path += file;
  if (!isValid()) {
    path = save.path;
    return false;
  }
  return true;
}

bool
Path::elideFile() {
  if (isDirectory()) 
    return false;
  size_t slashpos = path.rfind('/',path.size());
  if (slashpos == std::string::npos)
    return false;
  path.erase(slashpos+1);
  return true;
}

bool
Path::appendSuffix(const std::string& suffix) {
  if (isDirectory()) 
    return false;
  Path save(*this);
  path.append(".");
  path.append(suffix);
  if (!isValid()) {
    path = save.path;
    return false;
  }
  return true;
}

bool 
Path::elideSuffix() {
  if (isDirectory()) return false;
  size_t dotpos = path.rfind('.',path.size());
  size_t slashpos = path.rfind('/',path.size());
  if (slashpos != std::string::npos && dotpos != std::string::npos &&
      dotpos > slashpos) {
    path.erase(dotpos, path.size()-dotpos);
    return true;
  }
  return false;
}


bool
Path::createDirectory( bool create_parents) {
  // Make sure we're dealing with a directory
  if (!isDirectory()) return false;

  // Get a writeable copy of the path name
  char pathname[MAXPATHLEN];
  path.copy(pathname,MAXPATHLEN);

  // Null-terminate the last component
  int lastchar = path.length() - 1 ; 
  if (pathname[lastchar] == '/') 
    pathname[lastchar] = 0;
  else 
    pathname[lastchar+1] = 0;

  // If we're supposed to create intermediate directories
  if ( create_parents ) {
    // Find the end of the initial name component
    char * next = strchr(pathname,'/');
    if ( pathname[0] == '/') 
      next = strchr(&pathname[1],'/');

    // Loop through the directory components until we're done 
    while ( next != 0 ) {
      *next = 0;
      if (0 != access(pathname, F_OK | R_OK | W_OK))
        if (0 != mkdir(pathname, S_IRWXU | S_IRWXG))
          ThrowErrno(std::string(pathname) + ": Can't create directory");
      char* save = next;
      next = strchr(next+1,'/');
      *save = '/';
    }
  } 

  if (0 != access(pathname, F_OK | R_OK))
    if (0 != mkdir(pathname, S_IRWXU | S_IRWXG))
      ThrowErrno(std::string(pathname) + ": Can't create directory");
  return true;
}

bool
Path::createFile() {
  // Make sure we're dealing with a file
  if (!isFile()) return false; 

  // Create the file
  int fd = ::creat(path.c_str(), S_IRUSR | S_IWUSR);
  if (fd < 0)
    ThrowErrno(path + ": Can't create file");
  ::close(fd);

  return true;
}

bool
Path::createTemporaryFile() {
  // Make sure we're dealing with a file
  if (!isFile()) return false;

  // Append the filename filler
  char pathname[MAXPATHLEN];
  path.copy(pathname,MAXPATHLEN);
  pathname[path.length()] = 0;
  strcat(pathname,"XXXXXX");
  int fd = ::mkstemp(pathname);
  if (fd < 0) {
    ThrowErrno(path + ": Can't create temporary file");
  }
  path = pathname;
  ::close(fd);
  return true;
}

bool
Path::destroyDirectory(bool remove_contents) {
  // Make sure we're dealing with a directory
  if (!isDirectory()) return false;

  // If it doesn't exist, we're done.
  if (!exists()) return true;

  if (remove_contents) {
    // Recursively descend the directory to remove its content
    std::string cmd("/bin/rm -rf ");
    cmd += path;
    system(cmd.c_str());
  } else {
    // Otherwise, try to just remove the one directory
    char pathname[MAXPATHLEN];
    path.copy(pathname,MAXPATHLEN);
    int lastchar = path.length() - 1 ; 
    if (pathname[lastchar] == '/') 
      pathname[lastchar] = 0;
    else
      pathname[lastchar+1] = 0;
    if ( 0 != rmdir(pathname))
      ThrowErrno(std::string(pathname) + ": Can't destroy directory");
  }
  return true;
}

bool
Path::destroyFile() {
  if (!isFile()) return false;
  if (0 != unlink(path.c_str()))
    ThrowErrno(path + ": Can't destroy file");
  return true;
}

bool
Path::renameFile(const Path& newName) {
  if (!isFile()) return false;
  if (0 != rename(path.c_str(), newName.c_str()))
    ThrowErrno(std::string("can't rename ") + path + " as " + 
               newName.toString());
  return true;
}

bool
Path::setStatusInfo(const StatusInfo& si) const {
  if (!isFile()) return false;
  struct utimbuf utb;
  utb.actime = si.modTime.toPosixTime();
  utb.modtime = utb.actime;
  if (0 != ::utime(path.c_str(),&utb))
    ThrowErrno(path + ": can't set file modification time");
  if (0 != ::chmod(path.c_str(),si.mode))
    ThrowErrno(path + ": can't set mode");
  return true;
}

}

// vim: sw=2
