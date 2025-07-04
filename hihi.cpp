#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <fstream>
#include <vector>    
#include <sstream>   
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <filesystem>
#include <queue>

#pragma comment(lib, "ws2_32.lib")
using namespace std;
namespace fs = std::filesystem;

//Global variables for client state
bool g_prompt_confirmation = true; // Controls confirmation prompt for mget/mput
SOCKET g_control_sockfd = INVALID_SOCKET; // Main control socket for FTP server
bool g_is_binary_mode = true; // True for binary, false for ASCII. Default to binary.
bool g_passive_mode_preference = true; // True for passive (PASV), false for active (PORT). Client only supports PASV.
string g_log_filename = "ftp_client.log"; // Default log file name

// Function prototypes for new commands
void ftp_open(const string& ip, unsigned short port);
void ftp_close();
void ftp_ascii();
void ftp_binary();
void ftp_status();
void ftp_passive_toggle();
void display_help();

// Logging functions
void write_log(const string& message);
string get_timestamp();
void log_transfer(const string& operation, const string& filename, const string& status);
void log_scan(const string& filename, const string& scan_result);

// Logging implementation
string get_timestamp() {
    time_t now = time(0);
    struct tm timeinfo;
    localtime_s(&timeinfo, &now);

    ostringstream oss;
    oss << put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void write_log(const string& message) {
    ofstream logFile(g_log_filename, ios::app);
    if (logFile.is_open()) {
        logFile << "[" << get_timestamp() << "] " << message << endl;
        logFile.close();
    }
}

void log_transfer(const string& operation, const string& filename, const string& status) {
    string logMessage = operation + " - File: " + filename + " - Status: " + status;
    write_log(logMessage);
    cout << "LOG: " << logMessage << endl;
}

void log_scan(const string& filename, const string& scan_result) {
    string logMessage = "SCAN - File: " + filename + " - Result: " + scan_result;
    write_log(logMessage);
    cout << "LOG: " << logMessage << endl;
}

//Function to connect to a server
SOCKET connectToServer(const char* ip, unsigned short port) {
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCKET) {
        cerr << "Socket creation failed: " << WSAGetLastError() << "\n";
        return INVALID_SOCKET;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serverAddr.sin_addr);

    if (connect(sockfd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "Connect failed: " << WSAGetLastError() << "\n";
        closesocket(sockfd);
        return INVALID_SOCKET;
    }
    return sockfd;
}

//Function to parse PASV response
bool parsePasvResponse(const std::string& response, std::string& ip, int& port) {
    size_t start = response.find('(');
    size_t end = response.find(')');
    if (start == std::string::npos || end == std::string::npos) return false;
    std::string nums = response.substr(start + 1, end - start - 1);
    int h1, h2, h3, h4, p1, p2;
    if (sscanf_s(nums.c_str(), "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return false;
    ip = to_string(h1) + "." + to_string(h2) + "." + to_string(h3) + "." + to_string(h4);
    port = p1 * 256 + p2;
    return true;
}

//Function to send a command and receive response
void sendCommand(int sockfd, const string& cmd) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to server. Cannot send command.\n";
        return;
    }
    send(sockfd, cmd.c_str(), static_cast<int>(cmd.length()), 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }
}

//command "ls" : list all files in current directory
void ftp_ls(int controlSock) {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("LIST command failed - Not connected to server");
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for LIST.\n";
        write_log("LIST command failed - Not in passive mode");
        return;
    }

    write_log("LIST command initiated");

    send(controlSock, "PASV\r\n", 6, 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No PASV response.\n";
        write_log("LIST failed - No PASV response");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    string ip;
    int port;
    if (!parsePasvResponse(buffer, ip, port)) {
        cout << "Failed to parse PASV response.\n";
        write_log("LIST failed - Failed to parse PASV response");
        return;
    }

    int dataSock = static_cast<int>(connectToServer(ip.c_str(), port));
    if (dataSock == INVALID_SOCKET) {
        cout << "Failed to open data connection.\n";
        write_log("LIST failed - Failed to open data connection");
        return;
    }

    send(controlSock, "LIST\r\n", 6, 0);
    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }

    cout << "Directory listing:\n";
    while ((bytesReceived = recv(dataSock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[bytesReceived] = '\0';
        cout << buffer << flush;
    }
    cout << endl;

    closesocket(dataSock);

    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }

    write_log("LIST command completed successfully");
}

//command "pwd" : show current directory on server
void ftp_pwd(int sockfd) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("PWD command failed - Not connected to server");
        return;
    }

    write_log("PWD command initiated");

    send(sockfd, "PWD\r\n", 5, 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        string virtualPath;
        size_t firstQuote = string(buffer).find('"');
        size_t lastQuote = string(buffer).rfind('"');
        if (firstQuote != string::npos && lastQuote != string::npos && lastQuote > firstQuote) {
            virtualPath = string(buffer).substr(firstQuote + 1, lastQuote - firstQuote - 1);
            string nativeBase = "C:\\data";
            string nativePath = nativeBase + "\\" + virtualPath.substr(1);
            cout << "Mapped native path: " << nativePath << endl;
            write_log("PWD command completed - Current directory: " + virtualPath);
        }
    }
    else {
        write_log("PWD command failed - No response from server");
    }
}

//command "cd" : change directory on server
void ftp_cd(int sockfd, const string& dir) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("CD command failed - Not connected to server");
        return;
    }

    write_log("CD command initiated - Target directory: " + dir);

    string cmd = "CWD " + dir + "\r\n";
    send(sockfd, cmd.c_str(), static_cast<int>(cmd.length()), 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        if (strncmp(buffer, "250", 3) == 0) {
            write_log("CD command completed successfully - Changed to: " + dir);
        }
        else {
            write_log("CD command failed - Server response: " + string(buffer));
        }
    }
    else {
        write_log("CD command failed - No response from server");
    }
}

//command "lcd" : change local directory
void ftp_lcd(const string& localDir) {
    write_log("LCD command initiated - Target directory: " + localDir);

    wstring wideLocalDir(localDir.begin(), localDir.end());

    if (SetCurrentDirectory(wideLocalDir.c_str())) {
        cout << "Local directory changed to: " << localDir << endl;
        write_log("LCD command completed successfully - Changed to: " + localDir);
    }
    else {
        cout << "Failed to change local directory.\n";
        write_log("LCD command failed - Unable to change to: " + localDir);
    }
}

//command "lpwd" : local directory
void ftp_lpwd() {
    write_log("LPWD command initiated");

    wchar_t path[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, path)) {
        wcout << L"Local directory: " << path << endl;

        // Convert wide string to regular string for logging
        string pathStr(path, path + wcslen(path));
        write_log("LPWD command completed - Current local directory: " + pathStr);
    }
    else {
        wcout << L"Failed to get local directory.\n";
        write_log("LPWD command failed - Unable to get current directory");
    }
}

//command "mkdir" : create directory on server
void ftp_mkdir(int sockfd, const string& dirname) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("MKDIR command failed - Not connected to server");
        return;
    }

    write_log("MKDIR command initiated - Directory: " + dirname);

    string cmd = "MKD " + dirname + "\r\n";
    send(sockfd, cmd.c_str(), static_cast<int>(cmd.length()), 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        if (strncmp(buffer, "257", 3) == 0) {
            write_log("MKDIR command completed successfully - Created: " + dirname);
        }
        else {
            write_log("MKDIR command failed - Server response: " + string(buffer));
        }
    }
    else {
        write_log("MKDIR command failed - No response from server");
    }
}

//command "rmdir" : remove directory on server
void ftp_rmdir(int sockfd, const string& dirname) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("RMDIR command failed - Not connected to server");
        return;
    }

    write_log("RMDIR command initiated - Directory: " + dirname);

    string cmd = "RMD " + dirname + "\r\n";
    send(sockfd, cmd.c_str(), static_cast<int>(cmd.length()), 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        if (strncmp(buffer, "250", 3) == 0) {
            write_log("RMDIR command completed successfully - Removed: " + dirname);
        }
        else {
            write_log("RMDIR command failed - Server response: " + string(buffer));
        }
    }
    else {
        write_log("RMDIR command failed - No response from server");
    }
}

//command "delete" : delete file on server
void ftp_delete(int sockfd, const string& filename) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("DELETE command failed - Not connected to server");
        return;
    }

    write_log("DELETE command initiated - File: " + filename);

    string cmd = "DELE " + filename + "\r\n";
    send(sockfd, cmd.c_str(), static_cast<int>(cmd.length()), 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        if (strncmp(buffer, "250", 3) == 0) {
            write_log("DELETE command completed successfully - Deleted: " + filename);
        }
        else {
            write_log("DELETE command failed - Server response: " + string(buffer));
        }
    }
    else {
        write_log("DELETE command failed - No response from server");
    }
}

//command "rename" : rename file on server
void ftp_rename(int sockfd, const string& oldname, const string& newname) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_log("RENAME command failed - Not connected to server");
        return;
    }

    write_log("RENAME command initiated - From: " + oldname + " To: " + newname);

    string cmd1 = "RNFR " + oldname + "\r\n";
    send(sockfd, cmd1.c_str(), static_cast<int>(cmd1.length()), 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No response after RNFR.\n";
        write_log("RENAME command failed - No response after RNFR");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    if (strncmp(buffer, "350", 3) != 0) {
        cout << "RNFR failed. Aborting rename.\n";
        write_log("RENAME command failed - RNFR failed: " + string(buffer));
        return;
    }

    string cmd2 = "RNTO " + newname + "\r\n";
    send(sockfd, cmd2.c_str(), static_cast<int>(cmd2.length()), 0);

    memset(buffer, 0, sizeof(buffer));
    bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        if (strncmp(buffer, "250", 3) == 0) {
            write_log("RENAME command completed successfully - From: " + oldname + " To: " + newname);
        }
        else {
            write_log("RENAME command failed - RNTO failed: " + string(buffer));
        }
    }
    else {
        cout << "No response after RNTO.\n";
        write_log("RENAME command failed - No response after RNTO");
    }
}

//command "get/recv" : download single file from server
void ftp_get(int controlSock, const string& filename) {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for RETR.\n";
        return;
    }

    log_transfer("DOWNLOAD_START", filename, "Initiating download");

    send(controlSock, "PASV\r\n", 6, 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No PASV response.\n";
        log_transfer("DOWNLOAD_FAILED", filename, "No PASV response");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    string ip;
    int port;
    if (!parsePasvResponse(buffer, ip, port)) {
        cout << "Failed to parse PASV response.\n";
        log_transfer("DOWNLOAD_FAILED", filename, "Failed to parse PASV response");
        return;
    }

    SOCKET dataSock = connectToServer(ip.c_str(), port);
    if (dataSock == INVALID_SOCKET) {
        cout << "Failed to open data connection.\n";
        log_transfer("DOWNLOAD_FAILED", filename, "Failed to open data connection");
        return;
    }

    string retrCmd = "RETR " + filename + "\r\n";
    send(controlSock, retrCmd.c_str(), static_cast<int>(retrCmd.length()), 0);

    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;

        if (strncmp(buffer, "150", 3) != 0) {
            cout << "File transfer not started.\n";
            closesocket(dataSock);
            log_transfer("DOWNLOAD_FAILED", filename, "File transfer not started");
            return;
        }
    }

    FILE* file;
    fopen_s(&file, filename.c_str(), "wb");
    if (!file) {
        cout << "Failed to open local file for writing.\n";
        closesocket(dataSock);
        log_transfer("DOWNLOAD_FAILED", filename, "Failed to open local file for writing");
        return;
    }

    int totalBytes = 0;
    while ((bytesReceived = recv(dataSock, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytesReceived, file);
        totalBytes += bytesReceived;
    }

    fclose(file);
    closesocket(dataSock);

    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }

    cout << "File downloaded successfully: " << filename << endl;
    log_transfer("DOWNLOAD_SUCCESS", filename, "Downloaded " + to_string(totalBytes) + " bytes");
}

// command "put" : upload single file to server with ClamAV scan
void ftp_put(int controlSock, const string& filename) {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for STOR.\n";
        return;
    }

    log_transfer("UPLOAD_START", filename, "Initiating upload with ClamAV scan");

    // Step 1: Connect to ClamAV Agent and send file for scanning
    SOCKET clamSock = connectToServer("127.0.0.1", 9000);
    if (clamSock == INVALID_SOCKET) {
        cout << "Failed to connect to ClamAV Agent\n";
        log_scan(filename, "Failed to connect to ClamAV Agent");
        log_transfer("UPLOAD_FAILED", filename, "ClamAV connection failed");
        return;
    }

    log_scan(filename, "Connected to ClamAV Agent");

    string scanCmd = "SCAN " + filename + "\r\n";
    send(clamSock, scanCmd.c_str(), (int)scanCmd.length(), 0);

    char clamBuffer[1024] = {};
    int clamLen = recv(clamSock, clamBuffer, sizeof(clamBuffer) - 1, 0);
    if (clamLen <= 0) {
        cout << "No PASV response from ClamAV\n";
        closesocket(clamSock);
        log_scan(filename, "No PASV response from ClamAV");
        log_transfer("UPLOAD_FAILED", filename, "ClamAV scan failed");
        return;
    }
    clamBuffer[clamLen] = '\0';
    cout << "ClamAV: " << clamBuffer;

    string clamIp;
    int clamPort;
    if (!parsePasvResponse(clamBuffer, clamIp, clamPort)) {
        cout << "Invalid PASV response from ClamAV\n";
        closesocket(clamSock);
        log_scan(filename, "Invalid PASV response from ClamAV");
        log_transfer("UPLOAD_FAILED", filename, "ClamAV scan failed");
        return;
    }

    SOCKET clamDataSock = connectToServer(clamIp.c_str(), clamPort);
    if (clamDataSock == INVALID_SOCKET) {
        cout << "Failed to connect data socket to ClamAV\n";
        closesocket(clamSock);
        log_scan(filename, "Failed to connect data socket to ClamAV");
        log_transfer("UPLOAD_FAILED", filename, "ClamAV scan failed");
        return;
    }

    FILE* fileToScan;
    fopen_s(&fileToScan, filename.c_str(), "rb");
    if (!fileToScan) {
        cout << "Cannot open file: " << filename << endl;
        closesocket(clamSock);
        closesocket(clamDataSock);
        log_scan(filename, "Cannot open file for scanning");
        log_transfer("UPLOAD_FAILED", filename, "Cannot open file");
        return;
    }

    char fileBuffer[1024];
    int scannedBytes = 0;
    while (!feof(fileToScan)) {
        size_t n = fread(fileBuffer, 1, sizeof(fileBuffer), fileToScan);
        if (n > 0) {
            send(clamDataSock, fileBuffer, (int)n, 0);
            scannedBytes += n;
        }
    }

    fclose(fileToScan);
    closesocket(clamDataSock);

    clamLen = recv(clamSock, clamBuffer, sizeof(clamBuffer) - 1, 0);
    closesocket(clamSock);

    if (clamLen <= 0 || string(clamBuffer).find("OK") == string::npos) {
        cout << "ClamAV detected virus or scan failed. File not uploaded.\n";
        log_scan(filename, "VIRUS DETECTED or scan failed - " + string(clamBuffer));
        log_transfer("UPLOAD_FAILED", filename, "ClamAV scan failed or virus detected");
        return; // Exit if ClamAV detects a virus or fails to scan
    }

    cout << "ClamAV scan successful. Proceeding with FTP upload.\n";
    log_scan(filename, "CLEAN - Scanned " + to_string(scannedBytes) + " bytes");

    // Step 2: Enter passive mode with the FTP server
    send(controlSock, "PASV\r\n", 6, 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No PASV response from FTP server.\n";
        log_transfer("UPLOAD_FAILED", filename, "No PASV response from FTP server");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    // Step 3: Parse IP/Port from PASV response
    string ip;
    int port;
    if (!parsePasvResponse(buffer, ip, port)) {
        cout << "Failed to parse PASV response from FTP server.\n";
        log_transfer("UPLOAD_FAILED", filename, "Failed to parse PASV response from FTP server");
        return;
    }

    SOCKET dataSock = connectToServer(ip.c_str(), port);
    if (dataSock == INVALID_SOCKET) {
        cout << "Unable to establish data connection with FTP server.\n";
        log_transfer("UPLOAD_FAILED", filename, "Unable to establish data connection with FTP server");
        return;
    }

    // Step 4: Send STOR command to initiate upload
    string storCmd = "STOR " + filename + "\r\n";
    send(controlSock, storCmd.c_str(), static_cast<int>(storCmd.length()), 0);

    memset(buffer, 0, sizeof(buffer)); // Clear buffer before receiving
    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No response after STOR command.\n";
        closesocket(dataSock);
        log_transfer("UPLOAD_FAILED", filename, "No response after STOR command");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    if (strncmp(buffer, "150", 3) != 0) { // Check if server is ready to accept data
        cout << "FTP server rejected STOR command. Aborting upload.\n";
        closesocket(dataSock);
        log_transfer("UPLOAD_FAILED", filename, "FTP server rejected STOR command");
        return;
    }

    // Step 5: Open file and upload data to FTP server
    FILE* fileToUpload = nullptr;
    fopen_s(&fileToUpload, filename.c_str(), "rb");
    if (!fileToUpload) {
        cout << "Failed to open local file for FTP upload: " << filename << endl;
        closesocket(dataSock);
        log_transfer("UPLOAD_FAILED", filename, "Failed to open local file for FTP upload");
        return;
    }

    int uploadedBytes = 0;
    while (!feof(fileToUpload)) {
        size_t bytes = fread(fileBuffer, 1, sizeof(fileBuffer), fileToUpload);
        if (bytes > 0) {
            send(dataSock, fileBuffer, (int)bytes, 0);
            uploadedBytes += bytes;
        }
    }

    fclose(fileToUpload);
    closesocket(dataSock);

    // Step 6: Receive final FTP server response
    memset(buffer, 0, sizeof(buffer)); // Clear buffer before receiving
    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }
    else {
        cout << "No final response from FTP server after file transfer.\n";
    }

    cout << "File uploaded successfully: " << filename << endl;
    log_transfer("UPLOAD_SUCCESS", filename, "Uploaded " + to_string(uploadedBytes) + " bytes");
}

//command "mput" : upload multiple files to server
void ftp_mput(int controlSock, const std::vector<std::string>& filenames) {
    if (filenames.empty()) {
        cout << "No files specified for mput.\n";
        return;
    }
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for MPUT.\n";
        return;
    }

    write_log("MPUT operation started - " + to_string(filenames.size()) + " files");

    if (g_prompt_confirmation) {
        cout << "You are about to upload " << filenames.size() << " file(s):\n";
        for (const string& filename : filenames) {
            cout << "  - " << filename << "\n";
        }
        cout << "Do you want to proceed? (y/N): ";
        string confirmation;
        getline(cin, confirmation);

        if (confirmation != "y" && confirmation != "Y") {
            cout << "mput operation cancelled by user.\n";
            write_log("MPUT operation cancelled by user");
            return;
        }
    }

    cout << "Attempting to upload multiple files...\n";
    int successCount = 0;
    for (const string& filename : filenames) {
        cout << "\n--- Processing file: " << filename << " ---\n";
        ftp_put(controlSock, filename);
        cout << "--- Finished processing: " << filename << " ---\n";
        successCount++;
    }
    cout << "\nAll specified files processed for upload.\n";
    write_log("MPUT operation completed - " + to_string(successCount) + "/" + to_string(filenames.size()) + " files processed");
}

//command "mget" : download multiple files from server
void ftp_mget(int controlSock, const std::vector<std::string>& filenames) {
    if (filenames.empty()) {
        cout << "No files specified for mget.\n";
        return;
    }
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for MGET.\n";
        return;
    }

    write_log("MGET operation started - " + to_string(filenames.size()) + " files");

    if (g_prompt_confirmation) {
        cout << "You are about to download " << filenames.size() << " file(s):\n";
        for (const string& filename : filenames) {
            cout << "  - " << filename << "\n";
        }
        cout << "Do you want to proceed? (y/N): ";
        string confirmation;
        getline(cin, confirmation);

        if (confirmation != "y" && confirmation != "Y") {
            cout << "mget operation cancelled by user.\n";
            write_log("MGET operation cancelled by user");
            return;
        }
    }

    cout << "Attempting to download multiple files...\n";
    int successCount = 0;
    for (const string& filename : filenames) {
        cout << "\n--- Processing file: " << filename << " ---\n";
        ftp_get(controlSock, filename);
        cout << "--- Finished processing: " << filename << " ---\n";
        successCount++;
    }
    cout << "\nAll specified files processed for download.\n";
    write_log("MGET operation completed - " + to_string(successCount) + "/" + to_string(filenames.size()) + " files processed");
}

// open command: connect to an FTP server
void ftp_open(const string& ip, unsigned short port = 21) {
    if (g_control_sockfd != INVALID_SOCKET) {
        cout << "Already connected. Please 'close' current connection first.\n";
        return;
    }

    write_log("Attempting to connect to FTP server: " + ip + ":" + to_string(port));

    g_control_sockfd = connectToServer(ip.c_str(), port);
    if (g_control_sockfd == INVALID_SOCKET) {
        cerr << "Failed to connect to FTP server.\n";
        write_log("Failed to connect to FTP server: " + ip + ":" + to_string(port));
        return;
    }

    cout << "Connected to FTP server at " << ip << ":" << port << ".\n";
    write_log("Successfully connected to FTP server: " + ip + ":" + to_string(port));

    char buffer[1024] = { 0 };
    int bytesReceived = recv(g_control_sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }

    // Send default login commands
    sendCommand(g_control_sockfd, "USER user\r\n");
    sendCommand(g_control_sockfd, "PASS 14022006\r\n");
    write_log("Login completed with default credentials");

    // Set initial transfer mode on server
    if (g_is_binary_mode) {
        sendCommand(g_control_sockfd, "TYPE I\r\n");
        write_log("Transfer mode set to BINARY");
    }
    else {
        sendCommand(g_control_sockfd, "TYPE A\r\n");
        write_log("Transfer mode set to ASCII");
    }
}

// close command: disconnect from the FTP server
void ftp_close() {
    if (g_control_sockfd != INVALID_SOCKET) {
        sendCommand(g_control_sockfd, "QUIT\r\n");
        closesocket(g_control_sockfd);
        g_control_sockfd = INVALID_SOCKET;
        cout << "Disconnected from FTP server.\n";
        write_log("Disconnected from FTP server");
    }
    else {
        cout << "Not currently connected to an FTP server.\n";
    }
}

// ascii command: set file transfer mode to ASCII
void ftp_ascii() {
    if (g_control_sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }

    g_is_binary_mode = false;
    sendCommand(g_control_sockfd, "TYPE A\r\n");
    cout << "Transfer mode set to ASCII.\n";
    write_log("Transfer mode changed to ASCII");
}

// binary command: set file transfer mode to binary
void ftp_binary() {
    if (g_control_sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }

    g_is_binary_mode = true;
    sendCommand(g_control_sockfd, "TYPE I\r\n");
    cout << "Transfer mode set to binary.\n";
    write_log("Transfer mode changed to binary");
}

// status command: display client status and connection information
void ftp_status() {
    cout << "\n=== FTP Client Status ===" << endl;

    if (g_control_sockfd != INVALID_SOCKET) {
        cout << "Connection: Connected to FTP server" << endl;
    }
    else {
        cout << "Connection: Not connected" << endl;
    }

    cout << "Transfer mode: " << (g_is_binary_mode ? "Binary (TYPE I)" : "ASCII (TYPE A)") << endl;
    cout << "Passive mode: " << (g_passive_mode_preference ? "Enabled (PASV)" : "Disabled (PORT - Not supported)") << endl;
    cout << "Prompt confirmation: " << (g_prompt_confirmation ? "Enabled" : "Disabled") << endl;
    cout << "Log file: " << g_log_filename << endl;
    cout << "=========================" << endl;

    write_log("Status command executed - Mode: " + string(g_is_binary_mode ? "Binary" : "ASCII") +
        ", Passive: " + string(g_passive_mode_preference ? "On" : "Off") +
        ", Connected: " + string(g_control_sockfd != INVALID_SOCKET ? "Yes" : "No"));
}

// passive command: toggle passive mode preference
void ftp_passive_toggle() {
    g_passive_mode_preference = !g_passive_mode_preference;
    cout << "Passive mode " << (g_passive_mode_preference ? "enabled" : "disabled") << endl;

    if (!g_passive_mode_preference) {
        cout << "Warning: Active mode (PORT) is not supported by this client." << endl;
        cout << "Some operations may fail. Consider enabling passive mode." << endl;
    }

    write_log("Passive mode toggled - Now " + string(g_passive_mode_preference ? "enabled" : "disabled"));
}

// get all local files in a directory
vector<string> get_local_files(const string& directory, bool recursive = true) {
    vector<string> files;

    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path().string());
                }
            }
        }
        else {
            for (const auto& entry : fs::directory_iterator(directory)) {
                if (entry.is_regular_file()) {
                    files.push_back(entry.path().string());
                }
            }
        }
    }
    catch (const fs::filesystem_error& e) {
        cout << "Error reading directory: " << e.what() << endl;
        write_log("Error reading local directory: " + directory + " - " + e.what());
    }

    return files;
}

// create a remote directory recursively
bool create_remote_directory_recursive(int controlSock, const std::string& path) {
    if (controlSock == INVALID_SOCKET) return false;

    std::string current_path = "/";
    std::istringstream iss(path);
    std::string segment;

    while (std::getline(iss, segment, '/')) {
        if (segment.empty()) continue;

        if (current_path != "/") current_path += "/";
        current_path += segment;

        // Thử tạo thư mục (có thể đã tồn tại)
        std::string cmd = "MKD " + current_path + "\r\n";
        send(controlSock, cmd.c_str(), (int)cmd.length(), 0);

        char buffer[1024] = { 0 };
        recv(controlSock, buffer, sizeof(buffer) - 1, 0);
        // Không cần kiểm tra lỗi vì thư mục có thể đã tồn tại
    }

    return true;
}

// Upload files recursively from local directory to remote directory
void ftp_mput_recursive(int controlSock, const std::string& local_directory, const std::string& remote_directory = "") {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }

    if (!g_passive_mode_preference) {
        cout << "Error: Recursive upload requires passive mode.\n";
        return;
    }

    write_log("Recursive upload started - Directory: " + local_directory);

    // Lấy danh sách tất cả file trong thư mục
    std::vector<std::string> files = get_local_files(local_directory, true);

    if (files.empty()) {
        cout << "No files found in directory: " << local_directory << endl;
        return;
    }

    cout << "Found " << files.size() << " files for upload\n";

    if (g_prompt_confirmation) {
        cout << "Upload " << files.size() << " files recursively? (y/N): ";
        std::string confirm;
        getline(cin, confirm);
        if (confirm != "y" && confirm != "Y") {
            cout << "Upload cancelled.\n";
            return;
        }
    }

    // Tạo thư mục đích trên server nếu được chỉ định
    if (!remote_directory.empty()) {
        create_remote_directory_recursive(controlSock, remote_directory);
        ftp_cd(controlSock, remote_directory);
    }

    fs::path base_path(local_directory);
    int success_count = 0;

    for (const auto& file_path : files) {
        fs::path full_path(file_path);
        fs::path relative_path = fs::relative(full_path, base_path);

        // Tạo thư mục con trên server nếu cần
        if (relative_path.has_parent_path()) {
            std::string parent_dir = relative_path.parent_path().string();
            std::replace(parent_dir.begin(), parent_dir.end(), '\\', '/');
            create_remote_directory_recursive(controlSock, parent_dir);
        }

        // Upload file
        cout << "Uploading: " << relative_path << endl;

        // Chuyển đến thư mục chứa file
        if (relative_path.has_parent_path()) {
            std::string parent_dir = relative_path.parent_path().string();
            std::replace(parent_dir.begin(), parent_dir.end(), '\\', '/');
            ftp_cd(controlSock, parent_dir);
        }

        // Upload chỉ tên file
        std::string filename = relative_path.filename().string();

        // Tạm thời chuyển đến thư mục chứa file local
        fs::path old_path = fs::current_path();
        fs::current_path(full_path.parent_path());

        ftp_put(controlSock, filename);
        success_count++;

        // Trở về thư mục gốc
        fs::current_path(old_path);

        // Trở về thư mục gốc trên server
        if (!remote_directory.empty()) {
            ftp_cd(controlSock, remote_directory);
        }
        else {
            ftp_cd(controlSock, "/");
        }
    }

    cout << "Recursive upload completed: " << success_count << "/" << files.size() << " files\n";
    write_log("Recursive upload completed - " + std::to_string(success_count) + "/" + std::to_string(files.size()) + " files");
}

void ftp_mget_recursive(int controlSock, const std::string& remote_directory, const std::string& local_directory = ".") {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        return;
    }

    if (!g_passive_mode_preference) {
        cout << "Error: Recursive download requires passive mode.\n";
        return;
    }

    write_log("Recursive download started - Remote: " + remote_directory + ", Local: " + local_directory);

    // Tạo thư mục local nếu chưa tồn tại
    try {
        fs::create_directories(local_directory);
    }
    catch (const fs::filesystem_error& e) {
        cout << "Error creating local directory: " << e.what() << endl;
        return;
    }

    // Sử dụng queue để duyệt thư mục
    std::queue<std::pair<std::string, std::string>> dir_queue; // {remote_path, local_path}
    dir_queue.push({ remote_directory, local_directory });

    int file_count = 0;

    while (!dir_queue.empty()) {
        auto [current_remote, current_local] = dir_queue.front();
        dir_queue.pop();

        // Chuyển đến thư mục trên server
        ftp_cd(controlSock, current_remote);

        // Lấy danh sách file (giả sử có hàm parse danh sách từ LIST)
        cout << "Processing directory: " << current_remote << endl;

        // Tạo thư mục local tương ứng
        try {
            fs::create_directories(current_local);
        }
        catch (const fs::filesystem_error& e) {
            cout << "Error creating directory: " << e.what() << endl;
            continue;
        }

        // Chuyển đến thư mục local
        fs::path old_path = fs::current_path();
        fs::current_path(current_local);

        // Giả sử có danh sách file từ LIST command
        // Đây là phần cần implement parse LIST response
        cout << "Note: File listing and parsing needed for full implementation\n";

        // Trở về thư mục gốc
        fs::current_path(old_path);
    }

    write_log("Recursive download completed - " + std::to_string(file_count) + " files");
}


// help command: display available commands
void display_help() {
    cout << "\n=== FTP Client Commands ===" << endl;
    cout << "Connection Commands:" << endl;
    cout << "  open <ip> [port]     - Connect to FTP server (default port 21)" << endl;
    cout << "  close                - Disconnect from FTP server" << endl;
    cout << "  status               - Display client status" << endl;
    cout << "  quit/exit            - Exit the FTP client" << endl;
    cout << "" << endl;

    cout << "Transfer Mode Commands:" << endl;
    cout << "  ascii                - Set transfer mode to ASCII" << endl;
    cout << "  binary               - Set transfer mode to binary" << endl;
    cout << "  passive              - Toggle passive mode on/off" << endl;
    cout << "" << endl;

    cout << "Directory Commands:" << endl;
    cout << "  ls                   - List files in current remote directory" << endl;
    cout << "  pwd                  - Show current remote directory" << endl;
    cout << "  cd <directory>       - Change remote directory" << endl;
    cout << "  lcd <directory>      - Change local directory" << endl;
    cout << "  lpwd                 - Show current local directory" << endl;
    cout << "  mkdir <directory>    - Create directory on server" << endl;
    cout << "  rmdir <directory>    - Remove directory on server" << endl;
    cout << "" << endl;

    cout << "File Operations:" << endl;
    cout << "  get <filename>       - Download file from server" << endl;
    cout << "  put <filename>       - Upload file to server (with ClamAV scan)" << endl;
    cout << "  mget <file1> [file2] - Download multiple files" << endl;
    cout << "  mput <file1> [file2] - Upload multiple files" << endl;
    cout << "  delete <filename>    - Delete file on server" << endl;
    cout << "  rename <old> <new>   - Rename file on server" << endl;
    cout << "  rget <remote_dir> [local_dir] - Recursively download directory" << endl;
    cout << "  rput <local_dir> [remote_dir] - Recursively upload directory" << endl;
    cout << "" << endl;

    cout << "Other Commands:" << endl;
    cout << "  prompt               - Toggle confirmation prompts for mget/mput" << endl;
    cout << "  help/?               - Display this help message" << endl;
    cout << "============================" << endl;

    write_log("Help command executed");
}

// prompt command: toggle confirmation prompts for mget/mput
void ftp_prompt_toggle() {
    g_prompt_confirmation = !g_prompt_confirmation;
    cout << "Confirmation prompts " << (g_prompt_confirmation ? "enabled" : "disabled") << endl;
    write_log("Confirmation prompts toggled - Now " + string(g_prompt_confirmation ? "enabled" : "disabled"));
}

// Enhanced logging function to create log file if it doesn't exist
void initialize_log() {
    ofstream logFile(g_log_filename, ios::app);
    if (logFile.is_open()) {
        logFile << "\n[" << get_timestamp() << "] ========== FTP CLIENT SESSION START ==========" << endl;
        logFile.close();
        cout << "Log file initialized: " << g_log_filename << endl;
    }
    else {
        cout << "Warning: Could not create/open log file: " << g_log_filename << endl;
    }
}

// Function to close log session
void finalize_log() {
    ofstream logFile(g_log_filename, ios::app);
    if (logFile.is_open()) {
        logFile << "[" << get_timestamp() << "] ========== FTP CLIENT SESSION END ==========" << endl;
        logFile.close();
    }
}

// Enhanced version of existing log functions for better file transfer and scan logging
void log_transfer_detailed(const string& operation, const string& filename, const string& status, int bytes = 0, const string& additional_info = "") {
    ostringstream logMessage;
    logMessage << "TRANSFER - " << operation << " - File: " << filename << " - Status: " << status;

    if (bytes > 0) {
        logMessage << " - Bytes: " << bytes;
    }

    if (!additional_info.empty()) {
        logMessage << " - Info: " << additional_info;
    }

    write_log(logMessage.str());
    cout << "LOG: " << logMessage.str() << endl;
}

void log_scan_detailed(const string& filename, const string& scan_result, const string& scanner_info = "", int bytes_scanned = 0) {
    ostringstream logMessage;
    logMessage << "SCAN - File: " << filename << " - Result: " << scan_result;

    if (!scanner_info.empty()) {
        logMessage << " - Scanner: " << scanner_info;
    }

    if (bytes_scanned > 0) {
        logMessage << " - Bytes: " << bytes_scanned;
    }

    write_log(logMessage.str());
    cout << "LOG: " << logMessage.str() << endl;
}

// Function to log connection events
void log_connection(const string& event, const string& server_info, const string& status) {
    string logMessage = "CONNECTION - " + event + " - Server: " + server_info + " - Status: " + status;
    write_log(logMessage);
    cout << "LOG: " << logMessage << endl;
}

// Function to log command execution
void log_command(const string& command, const string& status, const string& details = "") {
    ostringstream logMessage;
    logMessage << "COMMAND - " << command << " - Status: " << status;

    if (!details.empty()) {
        logMessage << " - Details: " << details;
    }

    write_log(logMessage.str());
}

// Function to process user commands
void process_command(const string& input) {
    // Skip empty input
    if (input.empty()) {
        return;
    }

    // Parse command and arguments
    istringstream iss(input);
    string command;
    iss >> command;

    // Convert command to lowercase for case-insensitive matching
    transform(command.begin(), command.end(), command.begin(), ::tolower);

    // Handle commands
    if (command == "help" || command == "?") {
        display_help();
    }
    else if (command == "open") {
        string ip;
        int port = 21;
        iss >> ip;
        if (iss >> port) {
            // Port specified
        }

        if (ip.empty()) {
            cout << "Usage: open <ip> [port]" << endl;
            log_command("OPEN", "Failed - No IP specified");
        }
        else {
            ftp_open(ip, static_cast<unsigned short>(port));
        }
    }
    else if (command == "close") {
        ftp_close();
    }
    else if (command == "status") {
        ftp_status();
    }
    else if (command == "ascii") {
        ftp_ascii();
    }
    else if (command == "binary") {
        ftp_binary();
    }
    else if (command == "passive") {
        ftp_passive_toggle();
    }
    else if (command == "prompt") {
        ftp_prompt_toggle();
    }
    else if (command == "ls" || command == "dir") {
        ftp_ls(static_cast<int>(g_control_sockfd));
    }
    else if (command == "pwd") {
        ftp_pwd(static_cast<int>(g_control_sockfd));
    }
    else if (command == "lpwd") {
        ftp_lpwd();
    }
    else if (command == "cd") {
        string directory;
        iss >> directory;
        if (directory.empty()) {
            cout << "Usage: cd <directory>" << endl;
            log_command("CD", "Failed - No directory specified");
        }
        else {
            ftp_cd(static_cast<int>(g_control_sockfd), directory);
        }
    }
    else if (command == "lcd") {
        string directory;
        iss >> directory;
        if (directory.empty()) {
            cout << "Usage: lcd <directory>" << endl;
            log_command("LCD", "Failed - No directory specified");
        }
        else {
            ftp_lcd(directory);
        }
    }
    else if (command == "mkdir") {
        string directory;
        iss >> directory;
        if (directory.empty()) {
            cout << "Usage: mkdir <directory>" << endl;
            log_command("MKDIR", "Failed - No directory specified");
        }
        else {
            ftp_mkdir(static_cast<int>(g_control_sockfd), directory);
        }
    }
    else if (command == "rmdir") {
        string directory;
        iss >> directory;
        if (directory.empty()) {
            cout << "Usage: rmdir <directory>" << endl;
            log_command("RMDIR", "Failed - No directory specified");
        }
        else {
            ftp_rmdir(static_cast<int>(g_control_sockfd), directory);
        }
    }
    else if (command == "delete" || command == "del") {
        string filename;
        iss >> filename;
        if (filename.empty()) {
            cout << "Usage: delete <filename>" << endl;
            log_command("DELETE", "Failed - No filename specified");
        }
        else {
            ftp_delete(static_cast<int>(g_control_sockfd), filename);
        }
    }
    else if (command == "rename" || command == "ren") {
        string oldname, newname;
        iss >> oldname >> newname;
        if (oldname.empty() || newname.empty()) {
            cout << "Usage: rename <oldname> <newname>" << endl;
            log_command("RENAME", "Failed - Missing filename(s)");
        }
        else {
            ftp_rename(static_cast<int>(g_control_sockfd), oldname, newname);
        }
    }
    else if (command == "get" || command == "recv") {
        string filename;
        iss >> filename;
        if (filename.empty()) {
            cout << "Usage: get <filename>" << endl;
            log_command("GET", "Failed - No filename specified");
        }
        else {
            ftp_get(static_cast<int>(g_control_sockfd), filename);
        }
    }
    else if (command == "put" || command == "send") {
        string filename;
        iss >> filename;
        if (filename.empty()) {
            cout << "Usage: put <filename>" << endl;
            log_command("PUT", "Failed - No filename specified");
        }
        else {
            ftp_put(static_cast<int>(g_control_sockfd), filename);
        }
    }
    else if (command == "mget") {
        vector<string> filenames;
        string filename;
        while (iss >> filename) {
            filenames.push_back(filename);
        }
        if (filenames.empty()) {
            cout << "Usage: mget <filename1> [filename2] ..." << endl;
            log_command("MGET", "Failed - No filenames specified");
        }
        else {
            ftp_mget(static_cast<int>(g_control_sockfd), filenames);
        }
    }
    else if (command == "mput") {
        vector<string> filenames;
        string filename;
        while (iss >> filename) {
            filenames.push_back(filename);
        }
        if (filenames.empty()) {
            cout << "Usage: mput <filename1> [filename2] ..." << endl;
            log_command("MPUT", "Failed - No filenames specified");
        }
        else {
            ftp_mput(static_cast<int>(g_control_sockfd), filenames);
        }
    }
    else if (command == "user") {
        string username, password;
        iss >> username;

        if (username.empty()) {
            cout << "Usage: user <username>" << endl;
            log_command("USER", "Failed - No username specified");
            return;
        }

        cout << "Password: ";
        // Simple password input (note: this will show password on screen)
        // For production, consider using platform-specific hidden input
        getline(cin, password);

        if (g_control_sockfd == INVALID_SOCKET) {
            cout << "Not connected to a server." << endl;
            log_command("USER", "Failed - Not connected");
        }
        else {
            sendCommand(static_cast<int>(g_control_sockfd), "USER " + username + "\r\n");
            sendCommand(static_cast<int>(g_control_sockfd), "PASS " + password + "\r\n");
            log_command("USER", "Login attempt for user: " + username);
        }
    }
    else if (command == "quote") {
        string raw_command;
        getline(iss, raw_command);
        if (raw_command.empty()) {
            cout << "Usage: quote <raw_ftp_command>" << endl;
            log_command("QUOTE", "Failed - No command specified");
        }
        else {
            // Remove leading whitespace
            raw_command.erase(0, raw_command.find_first_not_of(" \t"));
            if (g_control_sockfd == INVALID_SOCKET) {
                cout << "Not connected to a server." << endl;
                log_command("QUOTE", "Failed - Not connected");
            }
            else {
                sendCommand(static_cast<int>(g_control_sockfd), raw_command + "\r\n");
                log_command("QUOTE", "Sent raw command: " + raw_command);
            }
        }
    }
    else if (command == "system") {
        if (g_control_sockfd == INVALID_SOCKET) {
            cout << "Not connected to a server." << endl;
            log_command("SYSTEM", "Failed - Not connected");
        }
        else {
            sendCommand(static_cast<int>(g_control_sockfd), "SYST\r\n");
            log_command("SYSTEM", "Requested system information");
        }
    }
    else if (command == "size") {
        string filename;
        iss >> filename;
        if (filename.empty()) {
            cout << "Usage: size <filename>" << endl;
            log_command("SIZE", "Failed - No filename specified");
        }
        else {
            if (g_control_sockfd == INVALID_SOCKET) {
                cout << "Not connected to a server." << endl;
                log_command("SIZE", "Failed - Not connected");
            }
            else {
                sendCommand(static_cast<int>(g_control_sockfd), "SIZE " + filename + "\r\n");
                log_command("SIZE", "Requested size for: " + filename);
            }
        }
    }
    else if (command == "noop") {
        if (g_control_sockfd == INVALID_SOCKET) {
            cout << "Not connected to a server." << endl;
            log_command("NOOP", "Failed - Not connected");
        }
        else {
            sendCommand(static_cast<int>(g_control_sockfd), "NOOP\r\n");
            log_command("NOOP", "Sent NOOP command");
        }
    }
    else if (command == "rget") {
        std::string remote_dir, local_dir;
        iss >> remote_dir >> local_dir;

        if (remote_dir.empty()) {
            cout << "Usage: rget <remote_directory> [local_directory]\n";
            return;
        }

        if (local_dir.empty()) local_dir = ".";

        ftp_mget_recursive(static_cast<int>(g_control_sockfd), remote_dir, local_dir);
    }
    else if (command == "rput") {
        std::string local_dir, remote_dir;
        iss >> local_dir >> remote_dir;

        if (local_dir.empty()) {
            cout << "Usage: rput <local_directory> [remote_directory]\n";
            return;
        }

        ftp_mput_recursive(static_cast<int>(g_control_sockfd), local_dir, remote_dir);
    }
    else {
        cout << "Unknown command: " << command << endl;
        cout << "Type 'help' or '?' for available commands" << endl;
        log_command("UNKNOWN", "Unknown command: " + command);
    }
}

// Refactored main function
int main() {
    // Initialize Winsock
    WSADATA wsaData;
    int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (result != 0) {
        cerr << "WSAStartup failed: " << result << endl;
        return 1;
    }

    // Initialize logging
    initialize_log();
    write_log("FTP Client application started");

    cout << "=== FTP Client ===" << endl;
    cout << "Type 'help' or '?' for available commands" << endl;
    cout << "Type 'quit' or 'exit' to close the application" << endl;
    cout << "==================" << endl;

    string input;
    while (true) {
        cout << "ftp> ";
        if (!getline(cin, input)) {
            break; // Handle EOF or input error
        }

        // Check for quit/exit commands
        if (!input.empty()) {
            istringstream iss(input);
            string command;
            iss >> command;
            transform(command.begin(), command.end(), command.begin(), ::tolower);

            if (command == "quit" || command == "exit") {
                log_command("QUIT", "User requested exit");
                break;
            }
        }

        // Process the command
        process_command(input);
    }

    // Cleanup
    cout << "\nClosing FTP client..." << endl;

    // Close any open connection
    if (g_control_sockfd != INVALID_SOCKET) {
        ftp_close();
    }

    // Finalize logging
    write_log("FTP Client application terminated");
    finalize_log();

    // Cleanup Winsock
    WSACleanup();

    cout << "Goodbye!" << endl;
    return 0;
}