//===- llvm/System/Path.h - Path Operating System Concept -------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file declares the llvm::sys::Path class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SYSTEM_PATH_H
#define LLVM_SYSTEM_PATH_H

#include <string>

namespace llvm {
namespace sys {

  /// This class provides an abstraction for the path to a file or directory 
  /// in the operating system's filesystem and provides various basic operations 
  /// on it.  Note that this class only represents the name of a path to a file
  /// or directory which may or may not be valid for a given machine's file 
  /// system. A Path ensures that the name it encapsulates is syntactical valid
  /// for the operating system it is running on but does not ensure correctness
  /// for any particular file system. A Path either references a file or a 
  /// directory and the distinction is consistently maintained. Most operations
  /// on the class have invariants that require the Path object to be either a
  /// file path or a directory path, but not both. Those operations will also 
  /// leave the object as either a file path or object path. There is exactly 
  /// one invalid Path which is the empty path. The class should never allow any
  /// other syntactically invalid non-empty path name to be assigned. Empty
  /// paths are required in order to indicate an error result. If the path is
  /// empty, the is_valid operation will return false. All operations will fail
  /// if is_valid is false. Operations that change the path will either return 
  /// false if it would cause a syntactically invalid path name (in which case 
  /// the Path object is left unchanged) or throw an std::string exception 
  /// indicating the error.
  /// @since 1.4
  /// @brief An abstraction for operating system paths.
  class Path {
    /// @name Constructors
    /// @{
    public:
      /// Construct a path to the root directory of the file system. The root
      /// directory is a top level directory above which there are no more 
      /// directories. For example, on UNIX, the root directory is /. On Windows
      /// it is C:\. Other operating systems may have different notions of
      /// what the root directory is.
      /// @throws nothing
      static Path GetRootDirectory();

      /// Construct a path to a unique temporary directory that is created in
      /// a "standard" place for the operating system. The directory is 
      /// guaranteed to be created on exit from this function. If the directory 
      /// cannot be created, the function will throw an exception.
      /// @throws std::string indicating why the directory could not be created.
      /// @brief Constrct a path to an new, unique, existing temporary
      /// directory.
      static Path GetTemporaryDirectory();

      /// Construct a path to the first system library directory. The
      /// implementation of Path on a given platform must ensure that this
      /// directory both exists and also contains standard system libraries
      /// suitable for linking into programs.
      /// @throws nothing
      /// @brief Construct a path to the first system library directory
      static Path GetSystemLibraryPath1();

      /// Construct a path to the second system library directory. The
      /// implementation of Path on a given platform must ensure that this
      /// directory both exists and also contains standard system libraries
      /// suitable for linking into programs. Note that the "second" system
      /// library directory may or may not be different from the first. 
      /// @throws nothing
      /// @brief Construct a path to the second system library directory
      static Path GetSystemLibraryPath2();

      /// Construct a path to the default LLVM configuration directory. The 
      /// implementation must ensure that this is a well-known (same on many
      /// systems) directory in which llvm configuration files exist. For 
      /// example, on Unix, the /etc/llvm directory has been selected.
      /// @throws nothing
      /// @brief Construct a path to the default LLVM configuration directory
      static Path GetLLVMDefaultConfigDir();

      /// Construct a path to the LLVM installed configuration directory. The
      /// implementation must ensure that this refers to the "etc" directory of
      /// the LLVM installation. This is the location where configuration files
      /// will be located for a particular installation of LLVM on a machine.
      /// @throws nothing
      /// @brief Construct a path to the LLVM installed configuration directory
      static Path GetLLVMConfigDir();

      /// Construct a path to the current user's home directory. The
      /// implementation must use an operating system specific mechanism for
      /// determining the user's home directory. For example, the environment 
      /// variable "HOME" could be used on Unix. If a given operating system 
      /// does not have the concept of a user's home directory, this static
      /// constructor must provide the same result as GetRootDirectory.
      /// @throws nothing
      /// @brief Construct a path to the current user's "home" directory
      static Path GetUserHomeDirectory();

      /// Return the suffix commonly used on file names that contain a shared
      /// object, shared archive, or dynamic link library. Such files are 
      /// linked at runtime into a process and their code images are shared 
      /// between processes. 
      /// @returns The dynamic link library suffix for the current platform.
      /// @brief Return the dynamic link library suffix.
      static std::string GetDLLSuffix();

      /// This is one of the very few ways in which a path can be constructed
      /// with a syntactically invalid name. The only *legal* invalid name is an 
      /// empty one. Other invalid names are not permitted. Empty paths are
      /// provided so that they can be used to indicate null or error results in
      /// other lib/System functionality.
      /// @throws nothing
      /// @brief Construct an empty (and invalid) path.
      Path() : path() {}

      /// This constructor will accept a std::string as a path but if verifies
      /// that the path string has a legal syntax for the operating system on
      /// which it is running. This allows a path to be taken in from outside
      /// the program. However, if the path is not valid, the Path object will
      /// be set to an empty string and an exception will be thrown.
      /// @throws std::string if the path string is not legal.
      /// @param unvalidated_path The path to verify and assign.
      /// @brief Construct a Path from a string.
      explicit Path(std::string unverified_path);

    /// @}
    /// @name Operators
    /// @{
    public:
      /// Makes a copy of \p that to \p this.
      /// @returns \p this
      /// @throws nothing
      /// @brief Assignment Operator
      Path & operator = ( const Path & that ) {
        path = that.path;
        return *this;
      }

      /// Compares \p this Path with \p that Path for equality.
      /// @returns true if \p this and \p that refer to the same thing.
      /// @throws nothing
      /// @brief Equality Operator
      bool operator == (const Path& that) const {
        return 0 == path.compare(that.path) ;
      }

      /// Compares \p this Path with \p that Path for inequality.
      /// @returns true if \p this and \p that refer to different things.
      /// @throws nothing
      /// @brief Inequality Operator
      bool operator !=( const Path & that ) const {
        return 0 != path.compare( that.path );
      }

      /// Determines if \p this Path is less than \p that Path. This is required
      /// so that Path objects can be placed into ordered collections (e.g.
      /// std::map). The comparison is done lexicographically as defined by
      /// the std::string::compare method.
      /// @returns true if \p this path is lexicographically less than \p that.
      /// @throws nothing
      /// @brief Less Than Operator
      bool operator< (const Path& that) const { 
        return 0 > path.compare( that.path ); 
      }

    /// @}
    /// @name Accessors
    /// @{
    public:
      /// This function will use an operating system specific algorithm to
      /// determine if the current value of \p this is a syntactically valid
      /// path name for the operating system. The path name does not need to
      /// exist, validity is simply syntactical. Empty paths are always invalid.
      /// @returns true iff the path name is syntactically legal for the 
      /// host operating system. 
      /// @brief Determine if a path is syntactically valid or not.
      bool is_valid() const;

      /// This function determines if the contents of the path name are
      /// empty. That is, the path has a zero length.
      /// @returns true iff the path is empty.
      /// @brief Determines if the path name is empty (invalid).
      bool is_empty() const { return path.empty(); }

      /// This function determines if the path name in this object is intended
      /// to reference a legal file name (as opposed to a directory name). This
      /// function does not verify anything with the file system, it merely
      /// determines if the syntax of the path represents a file name or not.
      /// @returns true if this path name references a file.
      /// @brief Determines if the path name references a file.
      bool is_file() const;

      /// This function determines if the path name in this object is intended
      /// to reference a legal directory name (as opposed to a file name). This
      /// function does not verify anything with the file system, it merely
      /// determines if the syntax of the path represents a directory name or
      /// not.
      /// @returns true if the path name references a directory
      /// @brief Determines if the path name references a directory.
      bool is_directory() const;

      /// This function determines if the path name in this object references
      /// the root (top level directory) of the file system. The details of what
      /// is considered the "root" may vary from system to system so this method
      /// will do the necessary checking. 
      /// @returns true iff the path name references the root directory.
      /// @brief Determines if the path references the root directory.
      bool is_root_directory() const;

      /// This function opens the file associated with the path name provided by 
      /// the Path object and reads its magic number. If the magic number at the
      /// start of the file matches \p magic, true is returned. In all other
      /// cases (file not found, file not accessible, etc.) it returns false.
      /// @returns true if the magic number of the file matches \p magic.
      /// @brief Determine if file has a specific magic number
      bool has_magic_number(const std::string& magic) const;

      /// This function determines if the path name in the object references an
      /// archive file by looking at its magic number.
      /// @returns true if the file starts with the magic number for an archive
      /// file.
      /// @brief Determine if the path references an archive file.
      bool is_archive() const;

      /// This function determines if the path name in the object references an
      /// LLVM Bytecode file by looking at its magic number.
      /// @returns true if the file starts with the magic number for LLVM 
      /// bytecode files.
      /// @brief Determine if the path references a bytecode file.
      bool is_bytecode_file() const;

      /// This function determines if the path name references an existing file
      /// or directory in the file system. Unlike is_file and is_directory, this
      /// function actually checks for the existence of the file or directory.
      /// @returns true if the pathname references an existing file.
      /// @brief Determines if the path is a file or directory in
      /// the file system.
      bool exists() const;

      /// This function determines if the path name references a readable file
      /// or directory in the file system. Unlike is_file and is_directory, this 
      /// function actually checks for the existence and readability (by the
      /// current program) of the file or directory.
      /// @returns true if the pathname references a readable file.
      /// @brief Determines if the path is a readable file or directory
      /// in the file system.
      bool readable() const;

      /// This function determines if the path name references a writable file
      /// or directory in the file system. Unlike is_file and is_directory, this 
      /// function actually checks for the existence and writability (by the
      /// current program) of the file or directory.
      /// @returns true if the pathname references a writable file.
      /// @brief Determines if the path is a writable file or directory
      /// in the file system.
      bool writable() const;

      /// This function determines if the path name references an executable 
      /// file in the file system. Unlike is_file and is_directory, this 
      /// function actually checks for the existence and executability (by 
      /// the current program) of the file.
      /// @returns true if the pathname references an executable file.
      /// @brief Determines if the path is an executable file in the file 
      /// system.
      bool executable() const;

      /// This function returns the current contents of the path as a
      /// std::string. This allows the underlying path string to be manipulated
      /// by other software.
      /// @returns std::string containing the path name.
      /// @brief Returns the path as a std::string.
      std::string get() const { return path; }

      /// This function returns the last component of the path name. If the
      /// is_directory() function would return true then this returns the name
      /// of the last directory in the path. If the is_file() function would
      /// return true then this function returns the name of the file without
      /// any of the preceding directories.
      /// @returns std::string containing the last component of the path name.
      /// @brief Returns the last component of the path name.
      std::string getLast() const;

      /// This function strips off the path and suffix of the file name and
      /// returns just the basename.
      /// @returns std::string containing the basename of the path
      /// @throws nothing
      /// @brief Get the base name of the path
      std::string get_basename() const;

      /// @returns a c string containing the path name.
      /// @brief Returns the path as a C string.
      const char* const c_str() const { return path.c_str(); }

    /// @}
    /// @name Mutators
    /// @{
    public:
      /// The path name is cleared and becomes empty. This is an invalid
      /// path name but is the *only* invalid path name. This is provided
      /// so that path objects can be used to indicate the lack of a 
      /// valid path being found.
      void clear() { path.clear(); }

      /// This method attempts to set the Path object to \p unverified_path
      /// and interpret the name as a directory name.  The \p unverified_path 
      /// is verified. If verification succeeds then \p unverified_path 
      /// is accepted as a directory and true is returned. Otherwise,
      /// the Path object remains unchanged and false is returned.
      /// @returns true if the path was set, false otherwise.
      /// @param unverified_path The path to be set in Path object.
      /// @throws nothing
      /// @brief Set a full path from a std::string
      bool set_directory(const std::string& unverified_path);

      /// This method attempts to set the Path object to \p unverified_path
      /// and interpret the name as a file name.  The \p unverified_path 
      /// is verified. If verification succeeds then \p unverified_path 
      /// is accepted as a file name and true is returned. Otherwise,
      /// the Path object remains unchanged and false is returned.
      /// @returns true if the path was set, false otherwise.
      /// @param unverified_path The path to be set in Path object.
      /// @throws nothing
      /// @brief Set a full path from a std::string
      bool set_file(const std::string& unverified_path);

      /// The \p dirname is added to the end of the Path if it is a legal
      /// directory name for the operating system. The precondition for this 
      /// function is that the Path must reference a directory name (i.e.
      /// is_directory() returns true).
      /// @param dirname A string providing the directory name to
      /// be added to the end of the path.
      /// @returns false if the directory name could not be added
      /// @throws nothing
      /// @brief Adds the name of a directory to a Path.
      bool append_directory( const std::string& dirname );

      /// One directory component is removed from the Path name. The Path must
      /// refer to a non-root directory name (i.e. is_directory() returns true
      /// but is_root_directory() returns false). Upon exit, the Path will 
      /// refer to the directory above it.
      /// @throws nothing
      /// @returns false if the directory name could not be removed.
      /// @brief Removes the last directory component of the Path.
      bool elide_directory();

      /// The \p filename is added to the end of the Path if it is a legal
      /// directory name for the operating system. The precondition for this
      /// function is that the Path reference a directory name (i.e. 
      /// is_directory() returns true).
      /// @throws nothing
      /// @returns false if the file name could not be added.
      /// @brief Appends the name of a file.
      bool append_file( const std::string& filename );

      /// One file component is removed from the Path name. The Path must
      /// refer to a file (i.e. is_file() returns true). Upon exit, 
      /// the Path will refer to the directory above it.
      /// @throws nothing
      /// @returns false if the file name could not be removed
      /// @brief Removes the last file component of the path.
      bool elide_file();

      /// A period and the \p suffix are appended to the end of the pathname.
      /// The precondition for this function is that the Path reference a file
      /// name (i.e. is_file() returns true). If the Path is not a file, no 
      /// action is taken and the function returns false. If the path would
      /// become invalid for the host operating system, false is returned.
      /// @returns false if the suffix could not be added, true if it was.
      /// @throws nothing
      /// @brief Adds a period and the \p suffix to the end of the pathname. 
      bool append_suffix(const std::string& suffix);

      /// The suffix of the filename is removed. The suffix begins with and
      /// includes the last . character in the filename after the last directory 
      /// separator and extends until the end of the name. If no . character is
      /// after the last directory separator, then the file name is left
      /// unchanged (i.e. it was already without a suffix) but the function return
      /// false.
      /// @returns false if there was no suffix to remove, true otherwise.
      /// @throws nothing
      /// @brief Remove the suffix from a path name.
      bool elide_suffix();

      /// This method attempts to create a directory in the file system with the
      /// same name as the Path object. The \p create_parents parameter controls
      /// whether intermediate directories are created or not. if \p
      /// create_parents is true, then an attempt will be made to create all
      /// intermediate directories. If \p create_parents is false, then only the
      /// final directory component of the Path name will be created. The 
      /// created directory will have no entries. 
      /// @returns false if the Path does not reference a directory, true 
      /// otherwise.
      /// @param create_parents Determines whether non-existent directory
      /// components other than the last one (the "parents") are created or not.
      /// @throws std::string if an error occurs.
      /// @brief Create the directory this Path refers to.
      bool create_directory( bool create_parents = false );

      /// This method attempts to create a file in the file system with the same 
      /// name as the Path object. The intermediate directories must all exist
      /// at the time this method is called. Use create_directories to 
      /// accomplish that. The created file will be empty upon return from this
      /// function.
      /// @returns false if the Path does not reference a file, true otherwise.
      /// @throws std::string if an error occurs.
      /// @brief Create the file this Path refers to.
      bool create_file();

      /// This method attempts to destroy the directory named by the last in 
      /// the Path name.  If \p remove_contents is false, an attempt will be 
      /// made to remove just the directory that this Path object refers to 
      /// (the final Path component). If \p remove_contents is true, an attempt
      /// will be made to remove the entire contents of the directory, 
      /// recursively. 
      /// @param destroy_contents Indicates whether the contents of a destroyed
      /// directory should also be destroyed (recursively). 
      /// @returns false if the Path does not refer to a directory, true 
      /// otherwise.
      /// @throws std::string if there is an error.
      /// @brief Removes the file or directory from the filesystem.
      bool destroy_directory( bool destroy_contents = false );

      /// This method attempts to destroy the file named by the last item in the 
      /// Path name. 
      /// @returns false if the Path does not refer to a file, true otherwise.
      /// @throws std::string if there is an error.
      /// @brief Destroy the file this Path refers to.
      bool destroy_file(); 

    /// @}
    /// @name Data
    /// @{
    private:
        std::string path; ///< Platform agnostic storage for the path name.

    /// @}
  };
}
}

// vim: sw=2

#endif
