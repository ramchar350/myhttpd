#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <dirent.h>
#include <time.h>
#include <float.h>
#include <arpa/inet.h>

#define MAX_REQUEST_SIZE 8192 // 8K buffer for the HTTP request
#define MAX_PATH_LENGTH 1024 // Maximum path length
#define DOCUMENT_ROOT "/homes/ramchar/cs252/lab5-src/http-root-dir/htdocs" // Root directory for serving files
#define BASIC_AUTH_REALM "MyHTTP-CS252" // Realm for basic auth
#define EXPECTED_AUTH "c3BhcmtydXNoOkdpJHBhcmtzMzUw" // base64 encoded sparkrush:Gi$parks350
#define DEFAULT_PORT 2350 // Default port if none is provided
#define THREAD_POOL_SIZE 5 // Number of threads in the pool

// Global variables for statistics
time_t server_start_time;  // When the server started
int total_requests = 0;    // Total number of requests
double min_service_time = DBL_MAX;  // Initialize to maximum possible value
double max_service_time = 0.0;      // Initialize to minimum possible value
char min_service_url[MAX_PATH_LENGTH] = "";  // URL with minimum service time
char max_service_url[MAX_PATH_LENGTH] = "";  // URL with maximum service time

// Mutex to protect access to the statistics
pthread_mutex_t stats_mutex = PTHREAD_MUTEX_INITIALIZER;

// Student names
const char* student_names = "Rushil Ramchand";

// File for logging
FILE* log_file = NULL;
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Global mutex for thread synchronization around accept()
pthread_mutex_t accept_mutex = PTHREAD_MUTEX_INITIALIZER;

// Function to check if a request has valid authentication
int checkAuthentication(const char* request) {
    // Look for auth header
    const char *auth = strstr(request, "Authorization: Basic ");

    // If no auth header found, return 0
    if (auth == NULL) {
        return 0;
    }

    // Move pointer to start of the credentials
    auth += 21; // Length of "Authorization: Basic "

    // Parse the credentials
    char credentials[256];
    int i = 0;
    while (auth[i] != '\r' && auth[i] != '\n' && auth[i] != '\0' && i < 255) {
        credentials[i] = auth[i];
        i++;
    }
    credentials[i] = '\0';

    // Compare with expected credentials
    if (strcmp(credentials, EXPECTED_AUTH) == 0) {
        return 1; // Authorized
    }

    return 0; // Unauthorized
}

// Determine content type based on file extension
const char* get_content_type(const char* path) {
    const char* ext = strchr(path, '.');

    if (ext == NULL) {
        return "application/octet-stream"; // Default binary type
    }

    ext++; // Move past the '.'

    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0) {
        return "text/html";
    } else if (strcasecmp(ext, "txt") == 0) {
        return "text/plain";
    } else if (strcasecmp(ext, "css") == 0) {
        return "text/css";
    } else if (strcasecmp(ext, "js") == 0) {
        return "application/javascript";
    } else if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
        return "image/jpeg";
    } else if (strcasecmp(ext, "png") == 0) {
        return "image/png";
    } else if (strcasecmp(ext, "gif") == 0) {
        return "image/gif";
    } else if (strcasecmp(ext, "svg") == 0) {
        return "image/svg+xml";
    } else if (strcasecmp(ext, "ico") == 0) {
        return "image/x-icon";
    } else if (strcasecmp(ext, "pdf") == 0) {
        return "application/pdf";
    } else if (strcasecmp(ext, "mp3") == 0) {
        return "audio/mpeg";
    } else if (strcasecmp(ext, "mp4") == 0) {
        return "video/mp4";
    }

    return "application/octet-stream";  // Default binary type
}

// Signal handler to reap zombie processes
void sigchld_handler(int s) {
    // Wait for all dead processes
    while(waitpid(-1, NULL, WNOHANG) > 0);
}

void sigint_handler(int s) {
    printf("\nShutting down server...\n");
    
    // Close the log file
    if (log_file != NULL) {
        fclose(log_file);
    }
    
    exit(0);
}

// Function to get the icon for a file type
const char* get_file_icon(const char* filename, int is_directory) {
    if (is_directory) {
        return "/icons/menu.gif";
    }
    
    // Get the file extension
    const char* ext = strrchr(filename, '.');
    if (ext == NULL) {
        return "/icons/unknown.gif"; // Default for unknown types
    }
    
    ext++; // Skip the dot
    
    // Match file extensions to icons
    if (strcasecmp(ext, "gif") == 0) {
        return "/icons/image.gif";
    }
    
    return "/icons/unknown.gif"; // Default for all other types
}

// Structure to store entry information for sorting
typedef struct {
    char name[256];
    int isDirectory;
    off_t size;
    time_t mtime;
} DirEntry;

// Comparison functions for sorting
int compare_by_name(const void *a, const void *b) {
    DirEntry *entryA = (DirEntry *)a;
    DirEntry *entryB = (DirEntry *)b;
    
    // Always list directories before files
    if (entryA->isDirectory && !entryB->isDirectory) return -1;
    if (!entryA->isDirectory && entryB->isDirectory) return 1;
    
    // If both are directories or both are files, sort by name
    return strcasecmp(entryA->name, entryB->name);
}

int compare_by_size(const void *a, const void *b) {
    DirEntry *entryA = (DirEntry *)a;
    DirEntry *entryB = (DirEntry *)b;
    
    // Always list directories before files
    if (entryA->isDirectory && !entryB->isDirectory) return -1;
    if (!entryA->isDirectory && entryB->isDirectory) return 1;
    
    // If both are directories, sort by name (directories don't have meaningful sizes)
    if (entryA->isDirectory && entryB->isDirectory) {
        return strcasecmp(entryA->name, entryB->name);
    }
    
    // If both are files, sort by size
    if (entryA->size < entryB->size) return -1;
    if (entryA->size > entryB->size) return 1;
    
    // If sizes are equal, sort by name as a tie-breaker
    return strcasecmp(entryA->name, entryB->name);
}

int compare_by_mtime(const void *a, const void *b) {
    DirEntry *entryA = (DirEntry *)a;
    DirEntry *entryB = (DirEntry *)b;
    
    // Always list directories before files
    if (entryA->isDirectory && !entryB->isDirectory) return -1;
    if (!entryA->isDirectory && entryB->isDirectory) return 1;
    
    // Sort by modification time
    if (entryA->mtime < entryB->mtime) return -1;
    if (entryA->mtime > entryB->mtime) return 1;
    
    // If times are equal, sort by name as a tie-breaker
    return strcasecmp(entryA->name, entryB->name);
}

// Function to send directory listing as HTML with sorting
void send_directory_listing(int client_socket, const char* dirPath, const char* urlPath, const char* query) {
    DIR *dir;
    struct dirent *entry;
    char htmlBuffer[MAX_REQUEST_SIZE];
    int htmlLength = 0;
    
    // Determine sort method from query parameter
    // Default is sort by name
    char sortMethod = 'N'; // N for name, S for size, M for modification time
    
    if (query != NULL) {
        if (strstr(query, "sort=S") != NULL) {
            sortMethod = 'S';
        } else if (strstr(query, "sort=M") != NULL) {
            sortMethod = 'M';
        }
    }
    
    // Open the directory
    dir = opendir(dirPath);
    if (dir == NULL) {
        // Failed to open directory
        const char* errorMessage = "<html><body><h1>403 Forbidden</h1><p>Cannot access directory.</p></body></html>";
        
        char responseHeader[MAX_REQUEST_SIZE];
        sprintf(responseHeader, "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n"
                "\r\n", strlen(errorMessage));
                
        write(client_socket, responseHeader, strlen(responseHeader));
        write(client_socket, errorMessage, strlen(errorMessage));
        return;
    }
    
    // Build array of directory entries for sorting
    DirEntry *entries = NULL;
    int numEntries = 0;
    int maxEntries = 256; // Initial capacity
    
    entries = (DirEntry *)malloc(maxEntries * sizeof(DirEntry));
    if (entries == NULL) {
        perror("malloc");
        closedir(dir);
        return;
    }
    
    // Read all entries
    while ((entry = readdir(dir)) != NULL) {
        // Skip the "." and ".." entries
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        // Check if we need to resize the array
        if (numEntries >= maxEntries) {
            maxEntries *= 2;
            DirEntry *newEntries = (DirEntry *)realloc(entries, maxEntries * sizeof(DirEntry));
            if (newEntries == NULL) {
                perror("realloc");
                free(entries);
                closedir(dir);
                return;
            }
            entries = newEntries;
        }
        
        // Get file information
        char entryPath[MAX_PATH_LENGTH];
        sprintf(entryPath, "%s/%s", dirPath, entry->d_name);
        
        struct stat entryStat;
        if (stat(entryPath, &entryStat) == -1) {
            // If we can't get stat info, still include in listing with defaults
            strncpy(entries[numEntries].name, entry->d_name, 255);
            entries[numEntries].name[255] = '\0';
            entries[numEntries].isDirectory = 0;
            entries[numEntries].size = 0;
            entries[numEntries].mtime = 0;
        } else {
            strncpy(entries[numEntries].name, entry->d_name, 255);
            entries[numEntries].name[255] = '\0';
            entries[numEntries].isDirectory = S_ISDIR(entryStat.st_mode);
            entries[numEntries].size = entryStat.st_size;
            entries[numEntries].mtime = entryStat.st_mtime;
        }
        
        numEntries++;
    }
    
    closedir(dir);
    
    // Sort entries based on the selected method
    if (sortMethod == 'S') {
        qsort(entries, numEntries, sizeof(DirEntry), compare_by_size);
    } else if (sortMethod == 'M') {
        qsort(entries, numEntries, sizeof(DirEntry), compare_by_mtime);
    } else {
        qsort(entries, numEntries, sizeof(DirEntry), compare_by_name);
    }
    
    // Start building the HTML
    htmlLength += sprintf(htmlBuffer + htmlLength, 
        "<html><head><title>Directory: %s</title></head><body>\n"
        "<h1>Directory Listing: %s</h1>\n"
        "<table border=\"1\">\n"
        "<tr>\n"
        "<th><a href=\"%s?sort=N\">Name</a></th>\n"
        "<th><a href=\"%s?sort=S\">Size</a></th>\n"
        "<th><a href=\"%s?sort=M\">Last Modified</a></th>\n"
        "</tr>\n", 
        urlPath, urlPath, urlPath, urlPath, urlPath);
    
    // Add parent directory link (..) if we're not at the root
    if (strcmp(urlPath, "/") != 0) {
        // Get the parent directory path
        char parentPath[MAX_PATH_LENGTH];
        strcpy(parentPath, urlPath);
        
        // Remove the trailing slash if present
        int len = strlen(parentPath);
        if (len > 0 && parentPath[len - 1] == '/') {
            parentPath[len - 1] = '\0';
        }
        
        // Find the last slash
        char* lastSlash = strrchr(parentPath, '/');
        if (lastSlash != NULL) {
            // If we found a slash, terminate the string there to get the parent path
            *(lastSlash + 1) = '\0';
        } else {
            // If no slash found, set to root
            strcpy(parentPath, "/");
        }
        
        // Include the current sort parameter in the parent link
        char parentUrl[MAX_PATH_LENGTH * 2];
        if (query != NULL && (sortMethod == 'S' || sortMethod == 'M')) {
            sprintf(parentUrl, "%s?sort=%c", parentPath, sortMethod);
        } else {
            strcpy(parentUrl, parentPath);
        }
        
        // Parent directory with icon
        htmlLength += sprintf(htmlBuffer + htmlLength,
            "<tr><td><a href=\"%s\"><img src=\"/icons/ball.gif\" alt=\"[PARENTDIR]\"> Parent Directory</a></td><td>-</td><td>-</td></tr>\n",
            parentUrl);
    }
    
    // Add entries in sorted order
    for (int i = 0; i < numEntries; i++) {
        // Format the size
        char sizeStr[20];
        if (entries[i].isDirectory) {
            strcpy(sizeStr, "-");
        } else {
            sprintf(sizeStr, "%ld", entries[i].size);
        }
        
        // Format the last modified time
        char timeStr[64];
        if (entries[i].mtime == 0) {
            strcpy(timeStr, "-");
        } else {
            strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&entries[i].mtime));
        }
        
        // Get the file icon
        const char* icon;
        if (entries[i].isDirectory) {
            icon = "/icons/menu.gif";
        } else {
            icon = get_file_icon(entries[i].name, 0);
        }
        
        // Build the entry URL
        char entryUrl[MAX_PATH_LENGTH * 2];
        if (urlPath[strlen(urlPath) - 1] == '/') {
            sprintf(entryUrl, "%s%s", urlPath, entries[i].name);
        } else {
            sprintf(entryUrl, "%s/%s", urlPath, entries[i].name);
        }
        
        // Include the current sort parameter in the URL for directories
        char entryUrlWithSort[MAX_PATH_LENGTH * 3];
        if (entries[i].isDirectory && query != NULL && (sortMethod == 'S' || sortMethod == 'M')) {
            sprintf(entryUrlWithSort, "%s/?sort=%c", entryUrl, sortMethod);
        } else if (entries[i].isDirectory) {
            sprintf(entryUrlWithSort, "%s/", entryUrl);
        } else {
            strcpy(entryUrlWithSort, entryUrl);
        }
        
        // Add the entry to the table
        htmlLength += sprintf(htmlBuffer + htmlLength, 
            "<tr><td><a href=\"%s\"><img src=\"%s\" alt=\"%s\"> %s%s</a></td><td>%s</td><td>%s</td></tr>\n",
            entryUrlWithSort,
            icon,
            entries[i].isDirectory ? "[DIR]" : "[   ]",
            entries[i].name, 
            entries[i].isDirectory ? "/" : "",
            sizeStr, 
            timeStr);
    }
    
    // Free the entries array
    free(entries);
    
    // Close the HTML
    htmlLength += sprintf(htmlBuffer + htmlLength, "</table></body></html>");
    
    // Send the HTTP response
    char responseHeader[MAX_REQUEST_SIZE];
    sprintf(responseHeader, "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", htmlLength);
            
    write(client_socket, responseHeader, strlen(responseHeader));
    write(client_socket, htmlBuffer, htmlLength);
    
    printf("Sent directory listing for %s with sort=%c\n", urlPath, sortMethod);
}

void handle_cgi_request(int client_socket, const char* path, const char* query) {
    printf("Handling CGI request: %s with query: %s\n", path, query);
    
    // Extract the script name from the path
    char scriptName[MAX_PATH_LENGTH];
    strcpy(scriptName, path + 9); // Skip "/cgi-bin/"
    
    // Build the full path to the CGI script - look in a sibling directory to htdocs
    char scriptPath[MAX_PATH_LENGTH];
    sprintf(scriptPath, "%s/../cgi-bin/%s", DOCUMENT_ROOT, scriptName);
    
    printf("Script path: %s\n", scriptPath);
    
    // Check if the script exists
    struct stat scriptInfo;
    if (stat(scriptPath, &scriptInfo) != 0) {
        printf("Script not found: %s\n", scriptPath);
        const char* errorMessage = "<html><body><h1>404 Not Found</h1><p>CGI script not found.</p></body></html>";
        
        char responseHeader[MAX_REQUEST_SIZE];
        sprintf(responseHeader, "HTTP/1.1 404 Not Found\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n"
                "\r\n", strlen(errorMessage));
                
        write(client_socket, responseHeader, strlen(responseHeader));
        write(client_socket, errorMessage, strlen(errorMessage));
        close(client_socket);
        return;
    }
    
    // Check if the script is executable (by owner, group, or others)
    if (!(scriptInfo.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        printf("Script not executable: %s (mode: %o)\n", scriptPath, scriptInfo.st_mode & 07777);
        const char* errorMessage = "<html><body><h1>403 Forbidden</h1><p>CGI script not executable.</p></body></html>";
        
        char responseHeader[MAX_REQUEST_SIZE];
        sprintf(responseHeader, "HTTP/1.1 403 Forbidden\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n"
                "\r\n", strlen(errorMessage));
                
        write(client_socket, responseHeader, strlen(responseHeader));
        write(client_socket, errorMessage, strlen(errorMessage));
        close(client_socket);
        return;
    }
    
    // Fork a child process to execute the CGI script
    pid_t pid = fork();
    
    if (pid < 0) {
        // Fork failed
        perror("fork");
        
        const char* errorMessage = "<html><body><h1>500 Internal Server Error</h1><p>Failed to execute CGI script.</p></body></html>";
        
        char responseHeader[MAX_REQUEST_SIZE];
        sprintf(responseHeader, "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n"
                "\r\n", strlen(errorMessage));
                
        write(client_socket, responseHeader, strlen(responseHeader));
        write(client_socket, errorMessage, strlen(errorMessage));
        close(client_socket);
        return;
    } else if (pid == 0) {
        // Child process
        
        // Send HTTP headers
        char header[MAX_REQUEST_SIZE];
        sprintf(header, "HTTP/1.1 200 Document follows\r\n"
                "Server: MyHTTP-CS252\r\n"
                "\r\n");
        write(client_socket, header, strlen(header));
        
        // Redirect stdout to client socket
        dup2(client_socket, STDOUT_FILENO);
        
        // Set up environment variables
        clearenv(); // Clear all environment variables
        setenv("REQUEST_METHOD", "GET", 1);
        setenv("SERVER_SOFTWARE", "MyHTTP-CS252", 1);
        setenv("SERVER_NAME", "localhost", 1);
        setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
        setenv("SERVER_PROTOCOL", "HTTP/1.1", 1);
        setenv("SERVER_PORT", "2350", 1);
        setenv("SCRIPT_NAME", path, 1);
        
        // Set the QUERY_STRING environment variable
        if (query != NULL && query[0] != '\0') {
            // Remove the leading '?' if present
            if (query[0] == '?') {
                setenv("QUERY_STRING", query + 1, 1);
            } else {
                setenv("QUERY_STRING", query, 1);
            }
        } else {
            setenv("QUERY_STRING", "", 1);
        }
        
        // Set up the arguments for execv
        char *args[2];
        args[0] = scriptPath;
        args[1] = NULL;
        
        // Execute the CGI script
        execv(scriptPath, args);
        
        // If execv fails
        perror("execv");
        fprintf(stderr, "Failed to execute CGI script: %s\n", scriptPath);
        
        // Send an error message to the client
        const char* execError = "<html><body><h1>500 Internal Server Error</h1><p>Failed to execute the CGI script.</p></body></html>";
        write(STDOUT_FILENO, execError, strlen(execError));
        
        exit(1);
    } else {
        // Parent process
        // Wait for the child to complete
        int status;
        waitpid(pid, &status, 0);
        
        printf("CGI script execution completed with status: %d\n", status);
        
        // Close client socket
        close(client_socket);
    }
}

void handle_stats_page(int client_socket) {
    // Record the start time passed from handle_client
    clock_t start_time = clock();
    
    // Calculate uptime
    time_t current_time = time(NULL);
    double uptime_seconds = difftime(current_time, server_start_time);
    
    int days = (int)(uptime_seconds / 86400);
    int hours = (int)((uptime_seconds - days * 86400) / 3600);
    int minutes = (int)((uptime_seconds - days * 86400 - hours * 3600) / 60);
    int seconds = (int)(uptime_seconds - days * 86400 - hours * 3600 - minutes * 60);
    
    // Format min and max service times
    char min_time_str[64];
    char max_time_str[64];
    
    pthread_mutex_lock(&stats_mutex);
    
    if (min_service_time == DBL_MAX) {
        sprintf(min_time_str, "No requests yet");
    } else {
        sprintf(min_time_str, "%.6f seconds for %s", min_service_time, min_service_url);
    }
    
    if (max_service_time == 0.0) {
        sprintf(max_time_str, "No requests yet");
    } else {
        sprintf(max_time_str, "%.6f seconds for %s", max_service_time, max_service_url);
    }
    
    // Get the current total_requests (including this request)
    int current_total_requests = total_requests;
    
    pthread_mutex_unlock(&stats_mutex);
    
    // Build the HTML response
    char html_buffer[MAX_REQUEST_SIZE];
    int html_length = 0;
    
    html_length += sprintf(html_buffer + html_length, 
        "<html><head><title>Server Statistics</title></head>\n"
        "<body>\n"
        "<h1>Server Statistics</h1>\n"
        "<table border=\"1\">\n"
        "<tr><th>Statistic</th><th>Value</th></tr>\n"
        "<tr><td>Student Names</td><td>%s</td></tr>\n"
        "<tr><td>Server Uptime</td><td>%d days, %d hours, %d minutes, %d seconds</td></tr>\n"
        "<tr><td>Total Requests</td><td>%d</td></tr>\n"
        "<tr><td>Minimum Service Time</td><td>%s</td></tr>\n"
        "<tr><td>Maximum Service Time</td><td>%s</td></tr>\n"
        "</table>\n"
        "</body></html>",
        student_names,
        days, hours, minutes, seconds,
        current_total_requests,
        min_time_str,
        max_time_str);
    
    // Send the HTTP response
    char response_header[MAX_REQUEST_SIZE];
    sprintf(response_header, "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n", html_length);
            
    write(client_socket, response_header, strlen(response_header));
    write(client_socket, html_buffer, html_length);
    
    // Update service stats for this request before closing
    clock_t end_time = clock();
    double service_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    pthread_mutex_lock(&stats_mutex);
    // Update min service time if this request was faster
    if (service_time < min_service_time) {
        min_service_time = service_time;
        strncpy(min_service_url, "/stats", MAX_PATH_LENGTH);
    }
    
    // Update max service time if this request was slower
    if (service_time > max_service_time) {
        max_service_time = service_time;
        strncpy(max_service_url, "/stats", MAX_PATH_LENGTH);
    }
    pthread_mutex_unlock(&stats_mutex);
    
    close(client_socket);
}

void handle_logs_page(int client_socket) {
    // First, we'll build an HTML response
    char html_header[4096];
    int header_length = 0;
    
    header_length += sprintf(html_header + header_length, 
        "<html><head><title>Server Logs</title></head>\n"
        "<body>\n"
        "<h1>Server Request Logs</h1>\n"
        "<table border=\"1\">\n"
        "<tr><th>Time</th><th>Source IP</th><th>Requested Path</th></tr>\n");
    
    // Send the HTTP response header first
    char response_header[1024];
    sprintf(response_header, "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Connection: close\r\n"
            "\r\n");
            
    write(client_socket, response_header, strlen(response_header));
    
    // Send the HTML header
    write(client_socket, html_header, header_length);
    
    // Read and send the log file contents line by line
    pthread_mutex_lock(&log_mutex);
    
    if (log_file != NULL) {
        // Close and reopen the file for reading
        fflush(log_file);
        FILE* read_log = fopen("server_log.txt", "r");
        
        if (read_log != NULL) {
            char line[1024];
            char time_str[64];
            char ip[64];
            char path[MAX_PATH_LENGTH];
            
            while (fgets(line, sizeof(line), read_log) != NULL) {
                // Parse the log entry
                if (sscanf(line, "[%63[^]]] %63s %1023[^\n]", time_str, ip, path) == 3) {
                    // Write each log entry as a table row
                    char row[2048];
                    int row_length = sprintf(row, "<tr><td>%s</td><td>%s</td><td>%s</td></tr>\n", 
                                           time_str, ip, path);
                    write(client_socket, row, row_length);
                }
            }
            
            fclose(read_log);
        }
    }
    
    pthread_mutex_unlock(&log_mutex);
    
    // Close the HTML
    const char* html_footer = "</table></body></html>";
    write(client_socket, html_footer, strlen(html_footer));
    
    close(client_socket);
}

void update_service_stats(clock_t start_time, const char* path) {
    // Calculate service time
    clock_t end_time = clock();
    double service_time = ((double)(end_time - start_time)) / CLOCKS_PER_SEC;
    
    // Update statistics
    pthread_mutex_lock(&stats_mutex);
    
    // Update min service time if this request was faster
    if (service_time < min_service_time) {
        min_service_time = service_time;
        strncpy(min_service_url, path, MAX_PATH_LENGTH);
    }
    
    // Update max service time if this request was slower
    if (service_time > max_service_time) {
        max_service_time = service_time;
        strncpy(max_service_url, path, MAX_PATH_LENGTH);
    }
    pthread_mutex_unlock(&stats_mutex);
}

// Function to handle a client request
void handle_client(int client_socket) {

    // Start timing this request
    clock_t start_time = clock();
    
    // Get client IP address
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    getpeername(client_socket, (struct sockaddr*)&client_addr, &client_len);
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
    
    // Increment request count ONCE per request
    pthread_mutex_lock(&stats_mutex);
    total_requests++;
    pthread_mutex_unlock(&stats_mutex);

    // Buffer to store the HTTP request
    char request[MAX_REQUEST_SIZE + 1];

    // Read the HTTP request
    memset(request, 0, sizeof(request)); // Clear the buffer
    int requestLength = 0;
    int bytesRead = 0;

    // Read bytes until we get a complete HTTP request
    while ((bytesRead = read(client_socket, request + requestLength, MAX_REQUEST_SIZE - requestLength)) > 0) {
        requestLength += bytesRead;

        // Check if we've reached the end of the HTTP request
        if (requestLength >= 4 && strstr(request, "\r\n\r\n") != NULL) {
            break;
        }

        // Check if buffer is full
        if (requestLength >= MAX_REQUEST_SIZE) {
            break;
        }
    }

    // Null-terminate the request string
    request[requestLength] = '\0';

    // Print the request for debugging
    printf("Received HTTP Request (%d bytes):\n%s\n", requestLength, request);

    // Extract the requested path and query
    char method[32];
    char pathWithQuery[MAX_PATH_LENGTH];
    char protocol[32];
    char defaultPath[] = "/index.html"; // Default path if none is specified

    sscanf(request, "%s %s %s", method, pathWithQuery, protocol);
    printf("Method: %s, Path with Query: %s, Protocol: %s\n", method, pathWithQuery, protocol);
    
    // Split path and query
    char path[MAX_PATH_LENGTH];
    char query[MAX_PATH_LENGTH] = "";
    
    char* queryStart = strchr(pathWithQuery, '?');
    if (queryStart != NULL) {
        // We have a query string
        int pathLength = queryStart - pathWithQuery;
        strncpy(path, pathWithQuery, pathLength);
        path[pathLength] = '\0';
        
        // Copy the query string (including the '?' character)
        strcpy(query, queryStart);
    } else {
        // No query string
        strcpy(path, pathWithQuery);
    }
    
    printf("Path: %s, Query: %s\n", path, query);

    // Prepare HTTP response header
    char responseHeader[MAX_REQUEST_SIZE];

    // Check for auth
    int isAuthenticated = checkAuthentication(request);

    if (!isAuthenticated) {
        // Send 401 message
        const char* unauthorizedMessage = "<html><body><h1>401 Unauthorized</h1><p>Authentication required to access this server.</p></body></html>";
        sprintf(responseHeader, "HTTP/1.1 401 Unauthorized\r\n"
                "WWW-Authenticate: Basic realm=\"%s\"\r\n"
                "Content-Type: text/html\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n"
                "\r\n", BASIC_AUTH_REALM, strlen(unauthorizedMessage));

        // Send the header and message
        write(client_socket, responseHeader, strlen(responseHeader));
        write(client_socket, unauthorizedMessage, strlen(unauthorizedMessage));

        printf("Authentication failed. Sent 401 Unauthorized response.\n");
        update_service_stats(start_time, path);
        close(client_socket);
        return;
    }

    printf("Authentication successful.\n");

    // Check for special paths
    if (strcmp(path, "/stats") == 0) {
        handle_stats_page(client_socket);
        return;
    }
    else if (strcmp(path, "/logs") == 0) {
        handle_logs_page(client_socket);
        update_service_stats(start_time, path); 
        return;
    }

    // If path is just "/", use the default path
    if (strcmp(path, "/") == 0) {
        strcpy(path, defaultPath);
    }

    // Prepare the full file path
    char filePath[MAX_PATH_LENGTH];

    // Check for icons
    if (strncmp(path, "/icons/", 7) == 0) {
        // Serve directly from the icons directory (sibling to htdocs)
        sprintf(filePath, "%s/../icons%s", DOCUMENT_ROOT, path + 6);
    }
    else {
        // Normal path handling
        sprintf(filePath, "%s%s", DOCUMENT_ROOT, path);
    }
    printf("Looking for file: %s\n", filePath);

    // Check if the path is a directory
    struct stat fileInfo;
    int isDirectory = 0;
    
    if (stat(filePath, &fileInfo) == 0) {
        // Check if it's a directory
        if (S_ISDIR(fileInfo.st_mode)) {
            isDirectory = 1;
        }
    }

    if (strncmp(path, "/cgi-bin/", 9) == 0) {
        // This is a CGI request
        handle_cgi_request(client_socket, path, query);
        update_service_stats(start_time, path);
        return;
    } 
    else if (isDirectory) {
        // Existing directory handling
        send_directory_listing(client_socket, filePath, path, query);
        update_service_stats(start_time, path);
        return;
    }
    else {
        // Check if the file exists and is readable
        int fileExists = 0;
        struct stat fileInfo;

        if (stat(filePath, &fileInfo) == 0 && (fileInfo.st_mode & S_IRUSR)) {
            fileExists = 1;
        }

        // Open the file if it exists
        int fileHandle = -1;
        long fileSize = 0;

        if (fileExists) {
            fileHandle = open(filePath, O_RDONLY);
            fileSize = fileInfo.st_size;
        }

        if (fileExists && fileHandle != -1) {
            // File exists and we can open it
            const char* contentType = get_content_type(filePath);
            sprintf(responseHeader, "HTTP/1.1 200 OK\r\n"
                "Content-Type: %s\r\n"
                "Content-Length: %ld\r\n"
                "Connection: close\r\n"
                "\r\n", contentType, fileSize);
                printf("File found. Sending 200 OK response with content type: %s\n", contentType);
        }
        else {
            // File doesn't exist or can't be opened
            const char* notFoundMessage = "<html><body><h1>404 Not Found</h1><p>The requested resource was not found on this server.</p></body></html>";
            sprintf(responseHeader, "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: %ld\r\n"
                    "Connection: close\r\n"
                    "\r\n", strlen(notFoundMessage));
            printf("File not found. Sending 404 Not Found response.\n");
        }

        // Send the response header
        write(client_socket, responseHeader, strlen(responseHeader));

        // Send the file content or not found message
        if (fileExists && fileHandle != -1) {
            // Send the file content
            char buffer[4096];
            int bytesRead;

            while ((bytesRead = read(fileHandle, buffer, sizeof(buffer))) > 0) {
                write(client_socket, buffer, bytesRead);
            }

            close(fileHandle);
        }
        else {
            // Send the not found message
            const char* notFoundMessage = "<html><body><h1>404 Not Found</h1><p>The requested resource was not found on this server.</p></body></html>";
            write(client_socket, notFoundMessage, strlen(notFoundMessage));
        }

        // Close the connection
        close(client_socket);
        printf("Connection closed.\n");
    }

    // Update service stats at the end (for file requests)
    update_service_stats(start_time, path);
    
    // Log the request
    pthread_mutex_lock(&log_mutex);
    if (log_file != NULL) {
        time_t now = time(NULL);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", localtime(&now));
        fprintf(log_file, "[%s] %s %s\n", time_str, client_ip, path);
        fflush(log_file); // Ensure it's written immediately
    }
    pthread_mutex_unlock(&log_mutex);
}

// Function for iterative server mode
void run_iterative_server(int masterSocket) {
    printf("Running in iterative server mode.\n");

    // Simple infinite loop to keep the server running
    while (1) {
        printf("\nWaiting for a new connection...\n");

        // Accept a new connection
        struct sockaddr_in clientIPAddress;
        int cliendAddressLength = sizeof(clientIPAddress);
        int slaveSocket = accept(masterSocket, (struct sockaddr *) &clientIPAddress,
                                 (socklen_t *) &cliendAddressLength);
        if (slaveSocket < 0) {
            perror("accept");
            continue;
        }

        printf("Connection accepted!\n");
        handle_client(slaveSocket);
    }
}

// Thread function to handle client requests
// Arg is pointer to client socket
void* thread_handle_client(void* arg) {
    int client_socket = *((int *)arg);

    // Free the memory allocated for the argument
    free(arg);

    // Set the thread as detached so its resources are auto released when it terminates
    pthread_detach(pthread_self());

    // Process the client request
    handle_client(client_socket);

    // Thread exits
    return NULL;
}

// Function for thread pool workers
void* thread_pool_worker(void* arg) {
    int masterSocket = *((int *)arg);

    printf("Thread %lu in the pool is ready to acccept connections.\n", (unsigned long)pthread_self());

    // Each thread runs a loop to handle connections
    while (1) {
        int slaveSocket;

        // Lock the mutex before calling accept
        pthread_mutex_lock(&accept_mutex);

        // Accept a new connection
        struct sockaddr_in clientIPAddress;
        int cliendAddressLength = sizeof(clientIPAddress);
        slaveSocket = accept(masterSocket, (struct sockaddr *) &clientIPAddress,
                                 (socklen_t *) &cliendAddressLength);
        
        // Unlock the mutex immediately after accepting a new connection
        pthread_mutex_unlock(&accept_mutex);

        if (slaveSocket < 0) {
            perror("accept");
            continue;
        }

        printf("Thread %lu accepted a connection.\n", (unsigned long)pthread_self());

        // Handle the client request
        handle_client(slaveSocket);
    }

    return NULL;
}

// Function for process-based server mode (-f)
void run_process_server(int masterSocket) {
    printf("Running in process-based concurrency mode (-f).\n");

    // Simple infinite loop to keep the server running
    while (1) {
        printf("\nWaiting for a new connection...\n");

        // Accept a new connection
        struct sockaddr_in clientIPAddress;
        int cliendAddressLength = sizeof(clientIPAddress);
        int slaveSocket = accept(masterSocket, (struct sockaddr *) &clientIPAddress,
                                 (socklen_t *) &cliendAddressLength);
        if (slaveSocket < 0) {
            perror("accept");
            continue;
        }

        printf("Connection accepted! Creating a new process to handle it.\n");

        // Fork a new process
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            close(slaveSocket);
        }
        else if (pid == 0) {
            // Child process
            close(masterSocket); // Child doesn't need the master socket
            handle_client(slaveSocket);
            exit(0); // Child exits after handling the request
        }
        else {
            // Parent process
            close(slaveSocket); // Parent doesn't need the slave socket
            // Parent continues to loop to accept more conditions
        }
    }
}

// Function for thread-based server mode (-t)
void run_thread_server(int masterSocket) {
    printf("Running in thread-based concurrency mode (-t).\n");

    // Simple infinite loop to keep the server running
    while (1) {
        printf("\nWaiting for a new connection...\n");

        // Accept a new connection
        struct sockaddr_in clientIPAddress;
        int cliendAddressLength = sizeof(clientIPAddress);
        int slaveSocket = accept(masterSocket, (struct sockaddr *) &clientIPAddress,
                                 (socklen_t *) &cliendAddressLength);
        if (slaveSocket < 0) {
            perror("accept");
            continue;
        }

        printf("Connection accepted! Creating a new thread to handle it.\n");

        // Allocate memory for the socket descriptor to pass to the thread
        int* client_sock = (int *)malloc(sizeof(int));
        *client_sock = slaveSocket;

        // Create a new thread to handle the client
        pthread_t thread_id;
        int thread_result = pthread_create(&thread_id, NULL, thread_handle_client, (void*)client_sock);

        if (thread_result != 0) {
            // Creation failed
            perror("pthread_create");
            close(slaveSocket);
            free(client_sock);
        }

        // Main thread continues to accept more connections & client socket is closed by the thread
    }
}

// Function for thread pool server mode (-p)
void run_thread_pool_server(int masterSocket) {
    printf("Running in thread pool concurrency mode (-p) with %d threads.\n", THREAD_POOL_SIZE);

    // Initialize thread pool
    pthread_t thread_pool[THREAD_POOL_SIZE];

    // Create the thread pool
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        // Pass the master socket to each thread
        int* socket_ptr = (int *)malloc(sizeof(int));
        *socket_ptr = masterSocket;

        int thread_result = pthread_create(&thread_pool[i], NULL, thread_pool_worker, socket_ptr);

        if (thread_result != 0) {
            perror("pthread_create");
            fprintf(stderr, "Failed to create thread %d in the pool\n", i);
            exit(1);
        }
    }

    printf("Thread pool created. Server is ready to accept connections.\n");

    // Wait indefinitely (the threads in the pool will handle all the connections)
    while (1) {
        sleep(60);
    }
}

int main(int argc, char **argv) {
    int port = DEFAULT_PORT;
    char mode = 'i'; // Default mode is iterative ('i')

    // Check command line args
    if (argc > 1) {
        // Check if first arg is a mode flag
        if (argv[1][0] == '-') {
            if (argv[1][1] == 'f' || argv[1][1] == 't' || argv[1][1] == 'p') {
                mode = argv[1][1];

                // If there's a port after the mode
                if (argc > 2) {
                    port = atoi(argv[2]);
                }
            }
            else {
                fprintf(stderr, "Invalid mode flag. Valid options are: -f, -t, -p\n");
                exit(-1);
            }
        }
        else {
            // First arg is not a flag
            port = atoi(argv[1]);
        }
    }

    // Valide port number
    if (port <= 1024 || port >= 65536) {
        fprintf(stderr, "Invalid port number. Must be between 1025 and 65535.\n");
        exit(-1);
    }

    printf("Setting up server on port %d...\n", port);

    // Initialize server start time
    server_start_time = time(NULL);

    // Open log file for appending
    log_file = fopen("server_log.txt", "a");
    if (log_file == NULL) {
        perror("Error opening log file");
        // Continue without logging rather than exiting
    }

    // Set up the signal handler for SIGCHILD
    if (mode == 'f') {
        struct sigaction sa;
        sa.sa_handler = sigchld_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        if (sigaction(SIGCHLD, &sa, NULL) == -1) {
            perror("sigaction");
            exit(1);
        }
    }

    signal(SIGINT, sigint_handler);
    
    // Set up server address structure
    struct sockaddr_in serverIPAddress;
    memset(&serverIPAddress, 0, sizeof(serverIPAddress));
    serverIPAddress.sin_family = AF_INET;
    serverIPAddress.sin_addr.s_addr = INADDR_ANY;
    serverIPAddress.sin_port = htons((u_short) port);
    
    // Create a master socket
    int masterSocket = socket(PF_INET, SOCK_STREAM, 0);
    if (masterSocket < 0) {
        perror("socket");
        exit(-1);
    }
    printf("Socket created successfully.\n");
    
    // Set socket option to reuse address
    int optval = 1;
    int err = setsockopt(masterSocket, SOL_SOCKET, SO_REUSEADDR, 
                         (char *) &optval, sizeof(int));
    if (err < 0) {
        perror("setsockopt");
        exit(-1);
    }
    
    // Bind the socket to IP address and port
    int error = bind(masterSocket, 
                     (struct sockaddr *)&serverIPAddress,
                     sizeof(serverIPAddress));
    if (error) {
        perror("bind");
        exit(-1);
    }
    printf("Socket bound successfully to port %d.\n", port);
    
    // Put socket in listening mode with queue length of 5
    int queueLength = 5;
    error = listen(masterSocket, queueLength);
    if (error) {
        perror("listen");
        exit(-1);
    }
    
    printf("Server listening on port %d...\n", port);
    
    // Keep the server running so we can test with netstat
    // printf("Server is now running. Press Ctrl+C to exit.\n");

    // Run appropriate server mode
    switch (mode) {
        case 'f':
            run_process_server(masterSocket);
            break;
        case 't':
            run_thread_server(masterSocket);
            break;
        case 'p':
            run_thread_pool_server(masterSocket);
            break;
        default:
            // Iterative
            run_iterative_server(masterSocket);
            break;
    }

    // Close the master socket
    close(masterSocket);
    return 0;
}