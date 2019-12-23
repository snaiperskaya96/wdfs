#include "wdfs.h"
#include "Fuse-impl.h"
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <string>
#include <vector>
#include <unordered_map>
#include "bridge.hpp"

#define DEBUG_LOGGING

#ifdef DEBUG_LOGGING
    #define LOG(fmt,...) printf(fmt, ##__VA_ARGS__)
#else
    #define LOG
#endif

// Value used for remote ID and local path mapping
struct id_cache_value {
    bool is_dir;
    std::string id;
};

// Value used for subfolder count caching
struct subfolder_cache_value {
    int validity;
    int subfolder_count;
};

// Value used for file size caching
struct filesize_cache_value {
    int validity;
    int filesize;
};

// Authorization header for https requests
std::string WdFs::auth_header = std::string("");
// Maps remote IDs to local paths
std::unordered_map<std::string, id_cache_value> remote_id_map;
// Caches to count of subfolders for a given remote ID of a folder
std::unordered_map<std::string, subfolder_cache_value> subfolder_count_cache;
// Maps a local path to a remote temp file's ID
std::unordered_map<std::string, std::string> temp_file_binding;
// Maps a local path to a newly created remote file's ID
std::unordered_map<std::string, std::string> create_opened_files;
// Used for caching entries of a specific parent entry
std::unordered_map<std::string, std::vector<entry_data>> list_entries_cache;
// Used by readdir for caching subfolder counts
std::unordered_map<std::string, int> list_entries_multiple_cache;
// Used for caching remote file sizes
std::unordered_map<std::string, filesize_cache_value> filesize_cache;

// Readonly open flag value
const int MY_O_RDONLY = 32768;

// Additional open flag values not in use currently
//const int MY_O_WRONLY = 32769;
//const int MY_O_RDWR = 32770;
//const int MY_O_APPEND = 33792;

// Set the auth header of the application
void WdFs::set_authorization_header(std::string authorization_header) {
    auth_header = authorization_header;
    LOG("Setting auth header to: %s\n", authorization_header.c_str());
}

// Split a string and get the individual parts
std::vector<std::string> split_string(const std::string &input, char delimiter) {
    std::vector<std::string> parts;
    int prev = 0;
    while (prev < input.size()) {
        int next_cut = input.find_first_of(delimiter, prev);
        if (next_cut == std::string::npos) { // Delimiter not in string
            parts.push_back(input.substr(prev, input.size() - prev));
            break;
        } else { // Delimiter is in the string
            parts.push_back(input.substr(prev, next_cut - prev));
            prev = next_cut + 1;
        }
    }
    return parts;
}

// List the entries of a full path
// Returns: 1 => folder found; 0 => folder not found, entry exists; -1 => entry doesn't exist
int list_entries_expand(const std::string &path, std::vector<entry_data> *result, const std::string &auth_header) {
    // Checks if the ID of the local path is cached
    if (remote_id_map.find(path) != remote_id_map.end()) {
        LOG("[list_entries_expand]: Corresponding ID for %s was found in the cache\n", path.c_str());
        LOG("[list_entries_expand]: Path's ID is: %s (%d)\n", remote_id_map[path].id.c_str(), remote_id_map[path].is_dir);
        if (!remote_id_map[path].is_dir) return 0;
        if (result != NULL) {
            std::string entryId = remote_id_map[path].id;
            std::vector<entry_data> cache_results;
            request_result res = list_entries(entryId.c_str(), auth_header, cache_results);
            LOG("[list_entries_expand]: Cached entry had %d entries\n", cache_results.size());
            if (res == REQUEST_SUCCESS) {
                LOG("[list_entries_expand]: list_entries_cache invalidated\n");
                *result = cache_results;
                list_entries_cache[entryId] = cache_results;
            } else if (res == REQUEST_CACHED) {
                LOG("[list_entries_expand]: list_entries_cache is valid\n");
                *result = list_entries_cache[entryId];
            }
        }
        return 1;
    }

    // Enumerates the IDs of parent folders
    std::vector<std::string> parts = split_string(path, '/');
    std::string current_id;
    std::vector<entry_data> current_items;
    std::string current_full_path;

    // Expand the folders starting from root to the requested path
    for (auto it = parts.begin(); it != parts.end(); ++it) {
        std::string current = *it;
        if (current.empty()) { // no current path so far
            current_id = "root";
        } else {
            current_full_path.append("/" + current);
            bool folder_found = false;
            bool entry_found = false;
            // find the name of the next folder in the path parts
            for (auto item = current_items.begin(); item != current_items.end(); ++item) {
                entry_data current_entry = *item;

                if (current_entry.name == current) { // name of the entry matches the name of the next folder
                    entry_found = true;
                    // Cache the ID of the entry
                    current_id = current_entry.id;
                    id_cache_value cache_value;
                    cache_value.id = current_entry.id;
                    cache_value.is_dir = current_entry.is_dir;
                    remote_id_map.insert(make_pair(current_full_path, cache_value));
                    if (current_entry.is_dir) { // The entry is a folder indeed
                        folder_found = true;
                        if (result == NULL) return 1;
                    }
                    break;
                }
            }
            if (!folder_found && !entry_found) return -1; // The entry doesn't exist on the server
            else if (!folder_found) return 0; // The entry exists but it's a file
        }

        current_items.clear();
        // List entries for the current path part
        request_result res = list_entries(current_id.c_str(), auth_header, current_items);
        if (res == REQUEST_CACHED) {
            current_items = list_entries_cache[current_id];
            LOG("[list_entries_expand]: expanding -> results taken from cache\n");
        } else if (res == REQUEST_SUCCESS) {
            list_entries_cache[current_id] = current_items;
            LOG("[list_entries_expand]: expanding -> results taken from server -> results cached\n");
        }
    }

    if (result != NULL) {
        *result = current_items;
    }

    return 1;
}

// Get the ID of the remote entry corresponding to the local path given
std::string get_path_remote_id(const std::string &path, const std::string &auth_header) {
    if (path == "/" || path == "") { // check if path is root
        return "root";
    } else if (remote_id_map.find(path) != remote_id_map.end()) { // check if path is in the cache
        LOG("[get_remote_id]: Path is cached in remote_id_map\n");
        return remote_id_map[path].id;
    }
    LOG("[get_remote_id]: Path isn't cached, fetching id from server\n");
    // list_entries_expand automatically populates the cache if the entry exists
    int expand_result = list_entries_expand(path, NULL, auth_header);
    LOG("[get_remote_id]: expand result: %d\n", expand_result);
    if (expand_result == 1 || expand_result == 0) { // Entry exists on server
        return get_path_remote_id(path, auth_header); // Will read from cache
    }

    return std::string("");
}

// Get the number of sub folders of a folder on the remote system
int get_subfolder_count(const std::string &path, const std::string &auth_header) {
    LOG("[get_subfolder_count]: Requesting subfolder count for %s\n", path.c_str());
    std::string remote_id = get_path_remote_id(path, auth_header);
    if (remote_id.empty()) return -2; // Server doesn't have this entry
    if (remote_id != "root" && !remote_id_map[path].is_dir) return -1; // Entry is not a directory
    if (subfolder_count_cache.find(remote_id) != subfolder_count_cache.end()) {
        // Check if there are results inserted by readdir
        subfolder_cache_value v = subfolder_count_cache[remote_id];
        if (v.validity == 1) {
            // Value is from readdir call, cache can be treated as valid
            v.validity = 0; // Invalidate cache after call
            return v.subfolder_count;
        }
    }
    std::vector<entry_data> entries;
    bool cache_invalidated = false;
    request_result res = list_entries(remote_id.c_str(), auth_header, entries);
    if (res == REQUEST_FAILED) return -2;
    else if (res == REQUEST_SUCCESS) {
        // current cache invalid
        LOG("[get_subfolder_count]: server returned entries result\n");
        list_entries_cache[remote_id] = entries;
        cache_invalidated = true;
    } else {
        // Request is cached, but not sure if subfolder_count is cached
        LOG("[get_subfolder_count]: Server returned result is cached\n");
    }
    if (subfolder_count_cache.find(remote_id) == subfolder_count_cache.end() || cache_invalidated) {
        // check if the cache needs to be updated and updated it
        LOG("[get_subfolder_count]: Subfolder count cache needs to be updated\n");
        int subfolder_count = 0;
        for (auto entry : list_entries_cache[remote_id]) {
            if (entry.is_dir) subfolder_count++;
        }
        LOG("[get_subfolder_count]: Pushing %s => %d to subfolder cache\n", remote_id.c_str(), subfolder_count);
        subfolder_cache_value v;
        v.subfolder_count = subfolder_count;
        v.validity = 0;
        subfolder_count_cache[remote_id] = v;
    }
    // subfolder_count cache is 100% up to date at this point
    return subfolder_count_cache[remote_id].subfolder_count;
}

// Get the size of a file on the remote system
int path_get_size(const std::string &file_path, const std::string &auth_header) {
    // get the remote ID of the file
    std::string file_id = get_path_remote_id(file_path, auth_header);
    if (file_id.empty()) return -1;
    int result = 0;
    // Check if cache is valid and return result from it if it is
    if (filesize_cache.find(file_id) != filesize_cache.end()) {
        filesize_cache_value v = filesize_cache[file_id];
        if (v.validity == 1) {
            // Cache has valid value from previous readdir call
            v.validity = 0; // Invalidate cache after this call
            return v.filesize;
        }
    }
    // Cache might not be valid
    request_result res = get_file_size(file_id, result, auth_header);
    if (res == REQUEST_SUCCESS) {
        // Server sent new file size, invalidated the cache
        filesize_cache[file_id].filesize = result;
    } else if (res == REQUEST_FAILED) return -1;

    // Cache is 100% valid at this point
    return filesize_cache[file_id].filesize;
}

// Release an open file
int WdFs::release(const char* file_path, struct fuse_file_info *) {
    LOG("[release]: Releasing file %s\n", file_path);
    std::string str_path(file_path);
    if (temp_file_binding.find(str_path) != temp_file_binding.end()) {
        // File to be released is an open temp file, close the write (upload) call here
        std::string file_name(str_path.substr(str_path.find_last_of('/') + 1));
        std::string remote_temp_id = temp_file_binding[str_path];
        bool close_result = file_write_close(remote_temp_id, auth_header);
        if (!close_result) LOG("[release]: Remote temp file close failed\n");
        else LOG("[release]: Remote temp file closed\n");
        temp_file_binding.erase(str_path);
        // Remove the original file
        std::string original_id = get_path_remote_id(str_path, auth_header);
        bool remove_result = remove_entry(original_id, auth_header);
        if (!remove_result) {
            LOG("[release]: Failed to remove old file!\n");
            return -1;
        }
        // Update the ID-local cache with the new ID of the old file
        id_cache_value val;
        val.is_dir = false;
        val.id = remote_temp_id;
        remote_id_map[str_path] = val;
        // Rename the new file
        bool rename_result = rename_entry(remote_temp_id, file_name, auth_header);
        if (!rename_result) {
            LOG("[release]: Failed to rename new file to old name!\n");
            return -1;
        }
    } else if (create_opened_files.find(str_path) != create_opened_files.end()) {
        // File is has been created, but hasn't been closed yet
        std::string new_file_id = create_opened_files[str_path];
        create_opened_files.erase(str_path);
        bool close_result = file_write_close(new_file_id, auth_header);
        if (!close_result) {
            LOG("[release]: Failed to close created file!\n");
            return -1;
        }
    }
    return 1337; // return value is ignored
}

// Open a file on the remote system
int WdFs::open(const char* file_path, struct fuse_file_info *fi) {
    LOG("[open]: Opening file %s\n", file_path);
    LOG("[open]: File opened with %d mode\n", fi->flags);
    // Ignore read only option as remote device is capable of handling offsets while reading
    if (fi->flags == MY_O_RDONLY) return 0;
    // tempfile required because remote can't write to a file after it's closed
    LOG("[open]: File wasn't in read only mode, preloading to remote temp file...\n");
    std::string str_path(file_path);
    std::string parent_path(str_path.substr(0, str_path.find_last_of('/')));
    std::string file_name(str_path.substr(str_path.find_last_of('/') + 1) + ".bridge_temp_file");
    LOG("[open]: Parent folder is: %s\n", parent_path.c_str());
    LOG("[open]: Temp file name is: %s\n", file_name.c_str());
    std::string parent_id = get_path_remote_id(parent_path, auth_header);
    LOG("[open]: Parent folder ID is: %s\n", parent_id.c_str());
    // Load contents of remote file into the temp file
    std::string remote_id = get_path_remote_id(str_path, auth_header);
    int remote_file_size = -1;
    // Get size of the remote file
    request_result res = get_file_size(remote_id, remote_file_size, auth_header);
    if (res == REQUEST_CACHED) remote_file_size = filesize_cache[remote_id].filesize; // Load size from cache
    else if (res == REQUEST_SUCCESS) filesize_cache[remote_id].filesize = remote_file_size; // Push new size to cache
    if (remote_file_size != -1) {
        LOG("[open]: Remote file exists and has %d bytes\n", remote_file_size);
        // Create temp file on remote
        std::string temp_file_id;
        bool temp_open_res = file_write_open(parent_id, file_name, auth_header, temp_file_id);
        if (!temp_open_res) return -1;
        // Read the remote file in chunks
        int bytes_read = 0;
        const int CHUNK_SIZE = 4096;
        void *buffer = malloc(CHUNK_SIZE);
        memset(buffer, 0, CHUNK_SIZE);
        std::string location_hdr("sdk/v2/files/" + temp_file_id);
        while (bytes_read != remote_file_size) {
            int local_read = 0;
            bool success = read_file(remote_id, buffer, bytes_read, CHUNK_SIZE, local_read, auth_header);
            LOG("[open]: Read %d bytes from remote; progress: %d/%d\n", local_read, bytes_read, remote_file_size);
            if (!success) return -1;
            bytes_read += local_read;
            // Write file to remote temp file
            bool write_result = write_file(auth_header, location_hdr, bytes_read - local_read, local_read, (char*) buffer);
            if (!write_result) return -1;
        }
        free(buffer);
        LOG("[open]: Remote file copied to temp file on the remote filesystem\n");
        // Bind path to temp file
        temp_file_binding.insert(make_pair(str_path, temp_file_id));
        LOG("[open]: Temp file binding %s=>%s cached\n", file_path, temp_file_id.c_str());
        return 0;
    }
    return -1;
}

// Write bytes to a file on the remote system
int WdFs::write(const char* file_path, const char* buffer, size_t size, off_t offset, struct fuse_file_info *) {
    LOG("[write]: Writing %d bytes of data at %d to %s\n", size, offset, file_path);
    std::string str_path(file_path);
    std::string file_id;
    if (temp_file_binding.find(str_path) != temp_file_binding.end()) {
        // We have a temp file that's open and has the contents of the real locked file
        file_id = temp_file_binding[str_path];
    } else {
        // We don't have a temp file => it's a newly created empty file that's still open for writing
        if (create_opened_files.find(str_path) == create_opened_files.end()) {
            LOG("[write]: Tried to write without tempfile and file's not in created_open map!\n");
            return -1;
        }
        file_id = create_opened_files[str_path];
    }
    // write the given bytes to the remote file
    LOG("[write]: Write target file found with ID: %s\n", file_id.c_str());
    std::string location_hdr("sdk/v2/files/" + file_id);
    bool result = write_file(auth_header, location_hdr, (int)offset, (int)size, buffer);
    if (!result) return -1;
    LOG("[write]: %d bytes written to %s\n", (int)size, file_path);
    return (int)size;
}

// Create a new file on the remote system
int WdFs::create(const char* file_path, mode_t mode, struct fuse_file_info *) {
    LOG("[create]: Creating file %s\n", file_path);
    std::string str_path(file_path);
    std::string parent_path(str_path.substr(0, str_path.find_last_of('/')));
    std::string file_name(str_path.substr(str_path.find_last_of('/') + 1));
    LOG("[create]: Parent folder is: %s\n", parent_path.c_str());
    LOG("[create]: File name is: %s\n", file_name.c_str());
    std::string parent_id = get_path_remote_id(parent_path, auth_header);
    LOG("[create]: Parent folder ID is: %s\n", parent_id.c_str());

    std::string new_id;
    bool open_result = file_write_open(parent_id, file_name, auth_header, new_id);
    if (!open_result) return -1;
    // Cache new file ID with the create map
    create_opened_files.insert(make_pair(str_path, new_id));
    LOG("[create]: ID of the new file is: %s\n", new_id.c_str());

    return 0;
}

// Remove a directory from the remote system
int WdFs::rmdir(const char* dir_path) {
    LOG("[rmdir]: Removing directory%s\n", dir_path);
    std::string str_path(dir_path);
    // Get ID of the remote directory
    std::string remote_entry_id = get_path_remote_id(str_path, auth_header);
    printf("[rmdir]: ID for remote entry is: %s\n", remote_entry_id.c_str());
    bool result = remove_entry(remote_entry_id, auth_header);
    if (result) {
        LOG("[rmdir]: Directory remove successful\n");
        // Remove folder from the ID cache
        remote_id_map.erase(str_path);
    } else {
        LOG("[rmdir]: Directory remove failed\n");
    }
    return 0;
}

// Remove a file from the remote system
int WdFs::unlink(const char* file_path) {
    LOG("[unlink]: Removing file %s\n", file_path);
    std::string str_path(file_path);
    // Get the ID of the remote file
    std::string remote_entry_id = get_path_remote_id(str_path, auth_header);
    printf("[unlink]: ID for remote entry is: %s\n", remote_entry_id.c_str());
    bool result = remove_entry(remote_entry_id, auth_header);
    if (result) {
        LOG("[unlink]: File remove successful\n");
        // Remove file from the ID cache
        remote_id_map.erase(str_path);
    } else {
        LOG("[unlink]: File remove failed\n");
    }
    return 0;
}

// Create a new directory
int WdFs::mkdir(const char* path, mode_t mode) {
    LOG("[mkdir]: Creating new directory for path %s\n", path);
    std::string str_path(path);
    int folder_name_index = str_path.find_last_of('/');
    std::string folder_name(str_path.substr(folder_name_index + 1, str_path.size() - folder_name_index - 1));
    std::string path_prefix(str_path.substr(0, folder_name_index));
    LOG("[mkdir]: Folder name is %s\n", folder_name.c_str());
    LOG("[mkdir]: Path prefix is %s\n", path_prefix.c_str());
    std::string prefix_id = get_path_remote_id(path_prefix, auth_header);
    LOG("[mkdir]: ID for path prefix is %s\n", prefix_id.c_str());
    std::string new_id = make_dir(folder_name.c_str(), prefix_id.c_str(), auth_header);
    // TODO: improve perfromacne by caching the new foled ID and the local path here
    LOG("[mkdir]: Finished with new folder ID: %s\n", new_id.c_str());
    return 0;
}

// Get the attributes of the file
int WdFs::getattr(const char *path, struct stat *st, struct fuse_file_info *) {
    LOG ("[getattr] called for path: %s\n", path);

    st->st_uid = getuid();
    st->st_gid = getgid();
    // TODO: legit timestamps here
    st->st_atime = time(NULL);
    st->st_mtime = time(NULL);

    std::string str_path(path);
    int subfolder_count = get_subfolder_count(str_path, auth_header);
    if (subfolder_count > -1) { // entry is a folder
        st->st_mode = S_IFDIR | 0755;
        LOG("[getattr] Path %s has %d subfolders\n", path, subfolder_count);
        // 2 + subfolder_count, because of the '.' and '..' special directories
        st->st_nlink = 2 + subfolder_count;
    } else if (subfolder_count == -1) { // entry is a file
        LOG("[getattr] Path %s is a file\n", path);
        st->st_mode = S_IFREG | 0644;
        st->st_nlink = 1;
        int file_size = path_get_size(str_path, auth_header);
        LOG("[getattr]: Size of %s is %d bytes\n", path, file_size);
        if (file_size == -1) return -ENOENT; // ID of the file is invalid or size can't be requested
        st->st_size = file_size;
    } else { // entry doesn't exist or is not listable by server becuase it's still open for writing
            if (create_opened_files.find(str_path) != create_opened_files.end()) {
                // This hack is required here, because the remote device doesn't list the file unless the write to it has been ended with file_write_close
                // The file is kept open after the create operation, because a write call might be the next and it's not possible to write to a remote file if it's been closed
                st->st_mode = S_IFREG | 0644;
                st->st_nlink = 1;
                st->st_size = 0;
            }
            else return -ENOENT;
    }
    return 0;
}

// List entries of a given directory
int WdFs::readdir(const char *path , void *buffer, fuse_fill_dir_t filler,
        off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    LOG("[readdir] Listing folder: %s\n", path);
    // These 2 paths are always there
    filler(buffer, ".", NULL, 0, FUSE_FILL_DIR_PLUS);
    filler(buffer, "..", NULL, 0, FUSE_FILL_DIR_PLUS);
    // Get entry list of the remote directory
    std::vector<entry_data> entries;
    std::string str_path(path);
    std::string subfolder_id_param;
    int expand_result = list_entries_expand(str_path, &entries, auth_header);
    if (expand_result == 1) { // has entry and it's a directory
        LOG("[readdir] Given path is a folder\n");
        for (int i = 0; i < entries.size(); i++) {
            // Insert entries to ID cache
            entry_data current = entries[i];
            std::string cache_key(str_path + (str_path == "/" ? "" : "/") + current.name);
            id_cache_value cache_value;
            cache_value.id = current.id;
            cache_value.is_dir = current.is_dir;
            remote_id_map.insert(make_pair(cache_key, cache_value));
            // Send the entry's name to the system
            filler(buffer, current.name.c_str(), NULL, 0, FUSE_FILL_DIR_PLUS);
            if (current.is_dir) { // Prepare subfolder count prefetching
                subfolder_id_param.append(current.id);
                subfolder_id_param.append(",");
                subfolder_cache_value v;
                v.validity = 1;
                v.subfolder_count = 0;
                subfolder_count_cache[current.id] = v;
            } else {
                // Cache prefetched file sizes
                filesize_cache_value v;
                v.validity = 1;
                v.filesize = current.size;
                filesize_cache[current.id] = v;
            }
        }

        LOG("[readdir.cache_remote_id] Listing remote id cache:\n");
        for (auto item : remote_id_map) {
            LOG("%s => %s\n", item.first.c_str(), item.second.id.c_str());
        }

        // Prefetch subfolder counts
        subfolder_id_param = subfolder_id_param.substr(0, subfolder_id_param.size() - 1);
        std::vector<entry_data> subfolders;
        request_result res = list_entries_multiple(subfolder_id_param.c_str(), auth_header, subfolders);
        if (res == REQUEST_SUCCESS) {
            LOG("[readdir.subfolder_count_prefetch]: Server sent subfolder data\n");
            for (auto entry : subfolders) {
                if (entry.is_dir) subfolder_count_cache[entry.parent_id].subfolder_count++;
            }
        } else if (res == REQUEST_CACHED) {
            // Fill subfolder_count from previously cached values
            LOG("[readdir.subfolder_count_prefetch]: Server sent cache is valid\n");
            for (auto item : list_entries_multiple_cache) {
                subfolder_count_cache[item.first].subfolder_count = item.second;
                subfolder_count_cache[item.first].validity = 1;
            }
        }

        LOG("[readdir.subfolder_count_prefetch] Listing subfolder count cache:\n");
        for (auto item : subfolder_count_cache) {
            LOG("%s => %d\n", item.first.c_str(), item.second.subfolder_count);
            if (res == REQUEST_SUCCESS) {
                // Save copy of original subfolder counts to be reused if list_entries_multiple is cached
                list_entries_multiple_cache[item.first] = item.second.subfolder_count;
            }
        }

        // Print the prefetched file sizes
        LOG("[readdir.filesize_count_prefetch] Listing file size cache:\n");
        for (auto item : filesize_cache) {
            LOG("%s => %d\n", item.first.c_str(), item.second.filesize);
        }
        return 0;
    } else if (expand_result == 0) {  // has entry but it's a file
        return 0;
    } else { // remote doesn't have the entry
        return -ENOENT;
    }
}

// Read the contents of a remote file
int WdFs::read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *) {
    LOG("[read]: Requesting content for file %s [%d:%d]\n", path, offset, offset + size);
    std::string str_path(path);
    std::string file_id = get_path_remote_id(str_path, auth_header);
    LOG("[read]: File ID on the remote is: %s\n", file_id.c_str());
    if (file_id.empty()) return -1;

    int bytes_read = 0;
    bool success = read_file(file_id, buffer, (int)offset, (int)size, bytes_read, auth_header);
    LOG("[read]: Actual bytes read from file: %d\n", bytes_read);
    if (!success) return -1;
    return bytes_read;
}