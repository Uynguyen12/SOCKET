#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <ctime>
#include <iomanip>

#pragma comment(lib, "ws2_32.lib")
using namespace std;

// Global variables for client state
bool g_prompt_confirmation = true; // Controls confirmation prompt for mget/mput
SOCKET g_control_sockfd = INVALID_SOCKET; // Main control socket for FTP server
bool g_is_binary_mode = true; // True for binary, false for ASCII. Default to binary.
bool g_passive_mode_preference = true; // True for passive (PASV), false for active (PORT). Client only supports PASV.

// Mutex for thread-safe logging
mutex g_log_mutex;

// Function to write to log file
void write_to_log(const string& operation, const string& filename, const string& status, const string& details = "") {
    lock_guard<mutex> lock(g_log_mutex);
    ofstream log_file("D:\\ftp_client.txt", ios::app);
    if (!log_file.is_open()) {
        cerr << "Failed to open log file for writing.\n";
        return;
    }

    auto now = time(nullptr);
    string timestamp = ctime(&now);
    timestamp.pop_back(); // Remove trailing newline

    log_file << "[" << timestamp << "] " << operation << ": " << filename
        << " - Status: " << status;
    if (!details.empty()) {
        log_file << " - Details: " << details;
    }
    log_file << "\n";
    log_file.close();
}

// Function prototypes for new commands
void ftp_open(const string& ip, unsigned short port);
void ftp_close();
void ftp_ascii();
void ftp_binary();
void ftp_status();
void ftp_passive_toggle();
void display_help();

// Function to connect to a server
SOCKET connectToServer(const char* ip, unsigned short port) {
    SOCKET sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == INVALID_SOCKET) {
        cerr << "Socket creation failed: " << WSAGetLastError() << "\n";
        write_to_log("Connect", ip, "Failed", "Socket creation error: " + to_string(WSAGetLastError()));
        return INVALID_SOCKET;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serverAddr.sin_addr);

    if (connect(sockfd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        cerr << "Connect failed: " << WSAGetLastError() << "\n";
        write_to_log("Connect", ip, "Failed", "Connection error: " + to_string(WSAGetLastError()));
        closesocket(sockfd);
        return INVALID_SOCKET;
    }
    write_to_log("Connect", ip, "Success", "Connected to " + string(ip) + ":" + to_string(port));
    return sockfd;
}

// Function to parse PASV response
bool parsePasvResponse(const std::string& response, std::string& ip, int& port) {
    size_t start = response.find('(');
    size_t end = response.find(')');
    if (start == std::string::npos || end == std::string::npos) {
        write_to_log("ParsePASV", "N/A", "Failed", "Invalid PASV response format");
        return false;
    }
    std::string nums = response.substr(start + 1, end - start - 1);
    int h1, h2, h3, h4, p1, p2;
    if (sscanf_s(nums.c_str(), "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
        write_to_log("ParsePASV", "N/A", "Failed", "Could not parse PASV response");
        return false;
    }
    ip = to_string(h1) + "." + to_string(h2) + "." + to_string(h3) + "." + to_string(h4);
    port = p1 * 256 + p2;
    write_to_log("ParsePASV", ip, "Success", "Parsed IP: " + ip + ", Port: " + to_string(port));
    return true;
}

// Function to send a command and receive response
void sendCommand(int sockfd, const string& cmd) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to server. Cannot send command.\n";
        write_to_log("SendCommand", cmd, "Failed", "No active connection");
        return;
    }
    send(sockfd, cmd.c_str(), static_cast<int>(cmd.length()), 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
        write_to_log("SendCommand", cmd, "Success", "Response: " + string(buffer));
    }
    else {
        write_to_log("SendCommand", cmd, "Failed", "No response received");
    }
}

// command "ls" : list all files in current directory
void ftp_ls(int controlSock) {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("List", "N/A", "Failed", "No active connection");
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for LIST.\n";
        write_to_log("List", "N/A", "Failed", "Client not in passive mode");
        return;
    }

    send(controlSock, "PASV\r\n", 6, 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No PASV response.\n";
        write_to_log("List", "N/A", "Failed", "No PASV response");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    string ip;
    int port;
    if (!parsePasvResponse(buffer, ip, port)) {
        cout << "Failed to parse PASV response.\n";
        return;
    }

    int dataSock = static_cast<int>(connectToServer(ip.c_str(), port));
    if (dataSock == INVALID_SOCKET) {
        cout << "Failed to open data connection.\n";
        write_to_log("List", "N/A", "Failed", "Could not open data connection");
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
    write_to_log("List", "N/A", "Success", "Directory listing received");

    closesocket(dataSock);

    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }
}

// command "pwd" : show current directory on server
void ftp_pwd(int sockfd) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("PWD", "N/A", "Failed", "No active connection");
        return;
    }
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
            cout << "Mapped SNative path: " << nativePath << endl;
            write_to_log("PWD", "N/A", "Success", "Current directory: " + virtualPath);
        }
        else {
            write_to_log("PWD", "N/A", "Failed", "Could not parse directory path");
        }
    }
    else {
        write_to_log("PWD", "N/A", "Failed", "No response received");
    }
}

// command "cd" : change directory on server
void ftp_cd(int sockfd, const string& dir) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("CWD", dir, "Failed", "No active connection");
        return;
    }
    sendCommand(sockfd, "CWD " + dir + "\r\n");
}

// command "lcd" : change local directory
void ftp_lcd(const string& localDir) {
    wstring wideLocalDir(localDir.begin(), localDir.end());

    if (SetCurrentDirectory(wideLocalDir.c_str())) {
        cout << "Local directory changed to: " << localDir << endl;
        write_to_log("LCD", localDir, "Success", "Changed to " + localDir);
    }
    else {
        cout << "Failed to change local directory.\n";
        write_to_log("LCD", localDir, "Failed", "Could not change directory");
    }
}

// command "lpwd" : local directory
void ftp_lpwd() {
    wchar_t path[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, path)) {
        wcout << L"Local directory: " << path << endl;
        wstring wpath(path);
        string spath(wpath.begin(), wpath.end());
        write_to_log("LPWD", spath, "Success", "Current directory: " + spath);
    }
    else {
        wcout << L"Failed to get local directory.\n";
        write_to_log("LPWD", "N/A", "Failed", "Could not get current directory");
    }
}

// command "mkdir" : create directory on server
void ftp_mkdir(int sockfd, const string& dirname) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("MKD", dirname, "Failed", "No active connection");
        return;
    }
    sendCommand(sockfd, "MKD " + dirname + "\r\n");
}

// command "rmdir" : remove directory on server
void ftp_rmdir(int sockfd, const string& dirname) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("RMD", dirname, "Failed", "No active connection");
        return;
    }
    sendCommand(sockfd, "RMD " + dirname + "\r\n");
}

// command "delete" : delete file on server
void ftp_delete(int sockfd, const string& filename) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("DELE", filename, "Failed", "No active connection");
        return;
    }
    sendCommand(sockfd, "DELE " + filename + "\r\n");
}

// command "rename" : rename file on server
void ftp_rename(int sockfd, const string& oldname, const string& newname) {
    if (sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("Rename", oldname + " to " + newname, "Failed", "No active connection");
        return;
    }
    string cmd1 = "RNFR " + oldname + "\r\n";
    send(sockfd, cmd1.c_str(), static_cast<int>(cmd1.length()), 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No response after RNFR.\n";
        write_to_log("Rename", oldname, "Failed", "No RNFR response");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    if (strncmp(buffer, "350", 3) != 0) {
        cout << "RNFR failed. Aborting rename.\n";
        write_to_log("Rename", oldname, "Failed", "RNFR rejected: " + string(buffer));
        return;
    }

    string cmd2 = "RNTO " + newname + "\r\n";
    send(sockfd, cmd2.c_str(), static_cast<int>(cmd2.length()), 0);

    memset(buffer, 0, sizeof(buffer));
    bytesReceived = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
        write_to_log("Rename", oldname + " to " + newname, "Success", "Response: " + string(buffer));
    }
    else {
        cout << "No response after RNTO.\n";
        write_to_log("Rename", oldname + " to " + newname, "Failed", "No RNTO response");
    }
}

// command "get/recv" : download single file from server
void ftp_get(int controlSock, const string& filename) {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("GET", filename, "Failed", "No active connection");
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for RETR.\n";
        write_to_log("GET", filename, "Failed", "Client not in passive mode");
        return;
    }

    send(controlSock, "PASV\r\n", 6, 0);

    char buffer[1024] = { 0 };
    int bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No PASV response.\n";
        write_to_log("GET", filename, "Failed", "No PASV response");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    string ip;
    int port;
    if (!parsePasvResponse(buffer, ip, port)) {
        cout << "Failed to parse PASV response.\n";
        return;
    }

    SOCKET dataSock = connectToServer(ip.c_str(), port);
    if (dataSock == INVALID_SOCKET) {
        cout << "Failed to open data connection.\n";
        write_to_log("GET", filename, "Failed", "Could not open data connection");
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
            write_to_log("GET", filename, "Failed", "Server rejected RETR: " + string(buffer));
            closesocket(dataSock);
            return;
        }
    }

    FILE* file;
    fopen_s(&file, filename.c_str(), "wb");
    if (!file) {
        cout << "Failed to open local file for writing.\n";
        write_to_log("GET", filename, "Failed", "Could not open local file");
        closesocket(dataSock);
        return;
    }

    size_t total_bytes = 0;
    while ((bytesReceived = recv(dataSock, buffer, sizeof(buffer), 0)) > 0) {
        fwrite(buffer, 1, bytesReceived, file);
        total_bytes += bytesReceived;
    }

    fclose(file);
    closesocket(dataSock);

    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }

    cout << "File downloaded successfully: " << filename << endl;
    write_to_log("GET", filename, "Success", "Downloaded " + to_string(total_bytes) + " bytes");
}

// command "put" : upload single file to server with ClamAV scan
void ftp_put(int controlSock, const string& filename) {
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("PUT", filename, "Failed", "No active connection");
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for STOR.\n";
        write_to_log("PUT", filename, "Failed", "Client not in passive mode");
        return;
    }

    // Step 1: Connect to ClamAV Agent and send file for scanning
    SOCKET clamSock = connectToServer("127.0.0.1", 9000);
    if (clamSock == INVALID_SOCKET) {
        cout << "Failed to connect to ClamAV Agent\n";
        write_to_log("ClamAV Scan", filename, "Failed", "Could not connect to ClamAV");
        return;
    }

    string scanCmd = "SCAN " + filename + "\r\n";
    send(clamSock, scanCmd.c_str(), (int)scanCmd.length(), 0);

    char clamBuffer[1024] = {};
    int clamLen = recv(clamSock, clamBuffer, sizeof(clamBuffer) - 1, 0);
    if (clamLen <= 0) {
        cout << "No PASV response from ClamAV\n";
        write_to_log("ClamAV Scan", filename, "Failed", "No PASV response");
        closesocket(clamSock);
        return;
    }
    clamBuffer[clamLen] = '\0';
    cout << "ClamAV: " << clamBuffer;

    string clamIp;
    int clamPort;
    if (!parsePasvResponse(clamBuffer, clamIp, clamPort)) {
        cout << "Invalid PASV response from ClamAV\n";
        write_to_log("ClamAV Scan", filename, "Failed", "Invalid PASV response");
        closesocket(clamSock);
        return;
    }

    SOCKET clamDataSock = connectToServer(clamIp.c_str(), clamPort);
    if (clamDataSock == INVALID_SOCKET) {
        cout << "Failed to connect data socket to ClamAV\n";
        write_to_log("ClamAV Scan", filename, "Failed", "Could not connect data socket");
        closesocket(clamSock);
        return;
    }

    FILE* fileToScan;
    fopen_s(&fileToScan, filename.c_str(), "rb");
    if (!fileToScan) {
        cout << "Cannot open file: " << filename << endl;
        write_to_log("ClamAV Scan", filename, "Failed", "Could not open file");
        closesocket(clamSock);
        closesocket(clamDataSock);
        return;
    }

    char fileBuffer[1024];
    size_t total_bytes_scanned = 0;
    while (!feof(fileToScan)) {
        size_t n = fread(fileBuffer, 1, sizeof(fileBuffer), fileToScan);
        if (n > 0) {
            send(clamDataSock, fileBuffer, (int)n, 0);
            total_bytes_scanned += n;
        }
    }

    fclose(fileToScan);
    closesocket(clamDataSock);

    clamLen = recv(clamSock, clamBuffer, sizeof(clamBuffer) - 1, 0);
    closesocket(clamSock);

    if (clamLen <= 0 || string(clamBuffer).find("OK") == string::npos) {
        cout << "ClamAV detected virus or scan failed. File not uploaded.\n";
        write_to_log("ClamAV Scan", filename, "Failed", "Scan result: " + string(clamBuffer));
        return; // Exit if ClamAV detects a virus or fails to scan
    }

    cout << "ClamAV scan successful. Proceeding with FTP upload.\n";
    write_to_log("ClamAV Scan", filename, "Success", "Scanned " + to_string(total_bytes_scanned) + " bytes");

    // Step 2: Enter passive mode with the FTP server
    send(controlSock, "PASV\r\n", 6, 0);
    char buffer[1024] = { 0 };
    int bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No PASV response from FTP server.\n";
        write_to_log("PUT", filename, "Failed", "No PASV response");
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    // Step 3: Parse IP/Port from PASV response
    string ip;
    int port;
    if (!parsePasvResponse(buffer, ip, port)) {
        cout << "Failed to parse PASV response from FTP server.\n";
        return;
    }

    SOCKET dataSock = connectToServer(ip.c_str(), port);
    if (dataSock == INVALID_SOCKET) {
        cout << "Unable to establish data connection with FTP server.\n";
        write_to_log("PUT", filename, "Failed", "Could not open data connection");
        return;
    }

    // Step 4: Send STOR command to initiate upload
    string storCmd = "STOR " + filename + "\r\n";
    send(controlSock, storCmd.c_str(), static_cast<int>(storCmd.length()), 0);

    memset(buffer, 0, sizeof(buffer)); // Clear buffer before receiving
    bytesReceived = recv(controlSock, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived <= 0) {
        cout << "No response after STOR command.\n";
        write_to_log("PUT", filename, "Failed", "No response after STOR");
        closesocket(dataSock);
        return;
    }
    buffer[bytesReceived] = '\0';
    cout << "Server: " << buffer;

    if (strncmp(buffer, "150", 3) != 0) { // Check if server is ready to accept data
        cout << "FTP server rejected STOR command. Aborting upload.\n";
        write_to_log("PUT", filename, "Failed", "Server rejected STOR: " + string(buffer));
        closesocket(dataSock);
        return;
    }

    // Step 5: Open file and upload data to FTP server
    FILE* fileToUpload = nullptr;
    fopen_s(&fileToUpload, filename.c_str(), "rb");
    if (!fileToUpload) {
        cout << "Failed to open local file for FTP upload: " << filename << endl;
        write_to_log("PUT", filename, "Failed", "Could not open local file");
        closesocket(dataSock);
        return;
    }

    size_t total_bytes_uploaded = 0;
    while (!feof(fileToUpload)) {
        size_t bytes = fread(fileBuffer, 1, sizeof(fileBuffer), fileToUpload);
        if (bytes > 0) {
            send(dataSock, fileBuffer, (int)bytes, 0);
            total_bytes_uploaded += bytes;
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
        write_to_log("PUT", filename, "Failed", "No final response after transfer");
    }

    cout << "File uploaded successfully: " << filename << endl;
    write_to_log("PUT", filename, "Success", "Uploaded " + to_string(total_bytes_uploaded) + " bytes");
}

// command "mput" : upload multiple files to server
void ftp_mput(int controlSock, const std::vector<std::string>& filenames) {
    if (filenames.empty()) {
        cout << "No files specified for mput.\n";
        write_to_log("MPUT", "Multiple files", "Failed", "No files specified");
        return;
    }
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("MPUT", "Multiple files", "Failed", "No active connection");
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for MPUT.\n";
        write_to_log("MPUT", "Multiple files", "Failed", "Client not in passive mode");
        return;
    }

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
            write_to_log("MPUT", "Multiple files", "Cancelled", "User cancelled operation");
            return;
        }
    }

    cout << "Attempting to upload multiple files...\n";
    write_to_log("MPUT", "Multiple files", "Started", "Uploading " + to_string(filenames.size()) + " files");
    for (const string& filename : filenames) {
        cout << "\n--- Processing file: " << filename << " ---\n";
        ftp_put(controlSock, filename);
        cout << "--- Finished processing: " << filename << " ---\n";
    }
    cout << "\nAll specified files processed for upload.\n";
    write_to_log("MPUT", "Multiple files", "Completed", "Processed " + to_string(filenames.size()) + " files");
}

// command "mget" : download multiple files from server
void ftp_mget(int controlSock, const std::vector<std::string>& filenames) {
    if (filenames.empty()) {
        cout << "No files specified for mget.\n";
        write_to_log("MGET", "Multiple files", "Failed", "No files specified");
        return;
    }
    if (controlSock == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("MGET", "Multiple files", "Failed", "No active connection");
        return;
    }
    if (!g_passive_mode_preference) {
        cout << "Error: Client is not in passive mode. Active mode (PORT) is not supported for MGET.\n";
        write_to_log("MGET", "Multiple files", "Failed", "Client not in passive mode");
        return;
    }

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
            write_to_log("MGET", "Multiple files", "Cancelled", "User cancelled operation");
            return;
        }
    }

    cout << "Attempting to download multiple files...\n";
    write_to_log("MGET", "Multiple files", "Started", "Downloading " + to_string(filenames.size()) + " files");
    for (const string& filename : filenames) {
        cout << "\n--- Processing file: " << filename << " ---\n";
        ftp_get(controlSock, filename);
        cout << "--- Finished processing: " << filename << " ---\n";
    }
    cout << "\nAll specified files processed for download.\n";
    write_to_log("MGET", "Multiple files", "Completed", "Processed " + to_string(filenames.size()) + " files");
}

// open command: connect to an FTP server
void ftp_open(const string& ip, unsigned short port = 21) {
    if (g_control_sockfd != INVALID_SOCKET) {
        cout << "Already connected. Please 'close' current connection first.\n";
        write_to_log("Open", ip, "Failed", "Already connected");
        return;
    }

    g_control_sockfd = connectToServer(ip.c_str(), port);
    if (g_control_sockfd == INVALID_SOCKET) {
        cerr << "Failed to connect to FTP server.\n";
        return;
    }

    cout << "Connected to FTP server at " << ip << ":" << port << ".\n";

    char buffer[1024] = { 0 };
    int bytesReceived = recv(g_control_sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesReceived > 0) {
        buffer[bytesReceived] = '\0';
        cout << "Server: " << buffer;
    }

    // Send default login commands
    sendCommand(g_control_sockfd, "USER user\r\n");
    sendCommand(g_control_sockfd, "PASS 14022006\r\n");

    // Set initial transfer mode on server
    if (g_is_binary_mode) {
        sendCommand(g_control_sockfd, "TYPE I\r\n");
    }
    else {
        sendCommand(g_control_sockfd, "TYPE A\r\n");
    }
}

// close command: disconnect from the FTP server
void ftp_close() {
    if (g_control_sockfd != INVALID_SOCKET) {
        sendCommand(g_control_sockfd, "QUIT\r\n");
        closesocket(g_control_sockfd);
        g_control_sockfd = INVALID_SOCKET;
        cout << "Disconnected from FTP server.\n";
        write_to_log("Close", "N/A", "Success", "Disconnected from server");
    }
    else {
        cout << "Not currently connected to an FTP server.\n";
        write_to_log("Close", "N/A", "Failed", "Not connected");
    }
}

// ascii command: set file transfer mode to ASCII
void ftp_ascii() {
    if (g_control_sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("ASCII", "N/A", "Failed", "No active connection");
        return;
    }
    sendCommand(g_control_sockfd, "TYPE A\r\n");
    g_is_binary_mode = false;
    cout << "File transfer mode set to ASCII.\n";
    write_to_log("ASCII", "N/A", "Success", "Set to ASCII mode");
}

// binary command: set file transfer mode to BINARY
void ftp_binary() {
    if (g_control_sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server.\n";
        write_to_log("BINARY", "N/A", "Failed", "No active connection");
        return;
    }
    sendCommand(g_control_sockfd, "TYPE I\r\n");
    g_is_binary_mode = true;
    cout << "File transfer mode set to BINARY.\n";
    write_to_log("BINARY", "N/A", "Success", "Set to BINARY mode");
}

// status command: show current session status
void ftp_status() {
    cout << "\n--- FTP Client Status ---\n";
    cout << "Connection: " << (g_control_sockfd != INVALID_SOCKET ? "Connected" : "Disconnected") << "\n";
    cout << "Transfer Mode: " << (g_is_binary_mode ? "BINARY" : "ASCII") << "\n";
    cout << "Passive Mode Preference: " << (g_passive_mode_preference ? "ON (Client will use PASV)" : "OFF (Client will attempt active mode, but not fully supported)") << "\n";
    cout << "Confirmation Prompt (mget/mput): " << (g_prompt_confirmation ? "ON" : "OFF") << "\n";
    cout << "Local Directory: ";
    wchar_t path[MAX_PATH];
    if (GetCurrentDirectoryW(MAX_PATH, path)) {
        wcout << path << "\n";
    }
    else {
        wcout << L"Failed to get local directory.\n";
    }
    cout << "-------------------------\n";
    wstring wpath(path);
    string spath(wpath.begin(), wpath.end());
    write_to_log("Status", "N/A", "Success", "Connection: " + string(g_control_sockfd != INVALID_SOCKET ? "Connected" : "Disconnected") +
        ", Mode: " + (g_is_binary_mode ? "BINARY" : "ASCII") +
        ", Passive: " + (g_passive_mode_preference ? "ON" : "OFF") +
        ", Prompt: " + (g_prompt_confirmation ? "ON" : "OFF") +
        ", Local Dir: " + spath);
}

// passive command: toggle passive FTP mode preference
void ftp_passive_toggle() {
    g_passive_mode_preference = !g_passive_mode_preference;
    cout << "Passive mode preference is now " << (g_passive_mode_preference ? "ON" : "OFF") << ".\n";
    if (!g_passive_mode_preference) {
        cout << "WARNING: Active mode (PORT) is not fully implemented in this client. File transfers may fail.\n";
    }
    write_to_log("Passive", "N/A", "Success", "Set to " + string(g_passive_mode_preference ? "ON" : "OFF"));
}

void display_help() {
    cout << "\n--- FTP Client Commands ---\n";
    cout << "ls                       : List files in current remote directory.\n";
    cout << "pwd                      : Show current remote directory.\n";
    cout << "cd <directory>           : Change remote directory.\n";
    cout << "lcd <local_directory>    : Change local directory.\n";
    cout << "lpwd                     : Show current local directory.\n";
    cout << "mkdir <dirname>          : Create directory on server.\n";
    cout << "rmdir <dirname>          : Remove directory on server.\n";
    cout << "delete <filename>        : Delete file on server.\n";
    cout << "rename <oldname> <newname> : Rename file on server.\n";
    cout << "get <filename>           : Download a single file.\n";
    cout << "recv <filename>          : Alias for 'get'.\n";
    cout << "put <filename>           : Upload a single file (scanned by ClamAV).\n";
    cout << "mget <file1> [file2...]  : Download multiple files.\n";
    cout << "mput <file1> [file2...]  : Upload multiple files (scanned by ClamAV).\n";
    cout << "prompt                   : Toggle confirmation prompt for mget/mput.\n";
    cout << "ascii                    : Set file transfer mode to ASCII.\n";
    cout << "binary                   : Set file transfer mode to BINARY.\n";
    cout << "status                   : Show current client session status.\n";
    cout << "passive                  : Toggle client's passive mode preference.\n";
    cout << "open <ip> [port]         : Connect to an FTP server (default port 21).\n";
    cout << "close                    : Disconnect from the current FTP server.\n";
    cout << "quit                     : Exit the FTP client.\n";
    cout << "bye                      : Alias for 'quit'.\n";
    cout << "help                     : Show this help message.\n";
    cout << "?                        : Alias for 'help'.\n";
    cout << "---------------------------\n";
    write_to_log("Help", "N/A", "Success", "Displayed command help");
}

// Function to handle FTP commands
void handleFtpCommand(const string& input) {
    if (input == "quit" || input == "bye") {
        ftp_close(); // Close connection if open
        return;
    }
    else if (input == "help" || input == "?") {
        display_help();
        return;
    }
    else if (input == "status") {
        ftp_status();
        return;
    }
    else if (input == "prompt") {
        g_prompt_confirmation = !g_prompt_confirmation;
        cout << "Confirmation prompt for mget/mput is now "
            << (g_prompt_confirmation ? "ON" : "OFF") << ".\n";
        write_to_log("Prompt", "N/A", "Success", "Set to " + string(g_prompt_confirmation ? "ON" : "OFF"));
        return;
    }
    else if (input == "ascii") {
        ftp_ascii();
        return;
    }
    else if (input == "binary") {
        ftp_binary();
        return;
    }
    else if (input == "passive") {
        ftp_passive_toggle();
        return;
    }
    else if (input.substr(0, 5) == "open ") {
        stringstream ss(input.substr(5));
        string ip_str;
        unsigned short port = 21; // Default FTP port
        ss >> ip_str;
        if (ss.peek() == ' ') {
            ss >> port;
        }
        ftp_open(ip_str, port);
        return;
    }
    else if (input == "close") {
        ftp_close();
        return;
    }
    else if (g_control_sockfd == INVALID_SOCKET) {
        cout << "Not connected to a server. Please use 'open' command to connect.\n";
        write_to_log("Command", input, "Failed", "No active connection");
        return;
    }

    // Commands that require an active connection
    if (input == "ls") {
        ftp_ls(g_control_sockfd);
    }
    else if (input.substr(0, 3) == "cd ") {
        ftp_cd(g_control_sockfd, input.substr(3));
    }
    else if (input == "pwd") {
        ftp_pwd(g_control_sockfd);
    }
    else if (input.substr(0, 4) == "lcd ") {
        ftp_lcd(input.substr(4));
    }
    else if (input == "lpwd") {
        ftp_lpwd();
    }
    else if (input.substr(0, 6) == "mkdir ") {
        ftp_mkdir(g_control_sockfd, input.substr(6));
    }
    else if (input.substr(0, 6) == "rmdir ") {
        ftp_rmdir(g_control_sockfd, input.substr(6));
    }
    else if (input.substr(0, 7) == "delete ") {
        ftp_delete(g_control_sockfd, input.substr(7));
    }
    else if (input.substr(0, 7) == "rename ") {
        size_t spacePos = input.find(' ', 7);
        if (spacePos != string::npos) {
            string oldname = input.substr(7, spacePos - 7);
            string newname = input.substr(spacePos + 1);
            if (!oldname.empty() && !newname.empty()) {
                ftp_rename(g_control_sockfd, oldname, newname);
            }
            else {
                cout << "Usage: rename <oldname> <newname>\n";
                write_to_log("Rename", oldname + " to " + newname, "Failed", "Invalid syntax");
            }
        }
        else {
            cout << "Usage: rename <oldname> <newname>\n";
            write_to_log("Rename", "N/A", "Failed", "Invalid syntax");
        }
    }
    else if (input.substr(0, 4) == "get ") {
        string filename = input.substr(4);
        if (!filename.empty())
            ftp_get(g_control_sockfd, filename);
        else {
            cout << "Usage: get <filename>\n";
            write_to_log("GET", "N/A", "Failed", "No filename specified");
        }
    }
    else if (input.substr(0, 5) == "recv ") {
        string filename = input.substr(5);
        if (!filename.empty())
            ftp_get(g_control_sockfd, filename);
        else {
            cout << "Usage: recv <filename>\n";
            write_to_log("RECV", "N/A", "Failed", "No filename specified");
        }
    }
    else if (input.substr(0, 4) == "put ") {
        string filename = input.substr(4);
        if (!filename.empty()) {
            ftp_put(g_control_sockfd, filename);
        }
        else {
            cout << "Usage: put <filename>\n";
            write_to_log("PUT", "N/A", "Failed", "No filename specified");
        }
    }
    else if (input.substr(0, 5) == "mput ") {
        string filenames_str = input.substr(5);
        if (filenames_str.empty()) {
            cout << "Usage: mput <file1> <file2> ...\n";
            write_to_log("MPUT", "N/A", "Failed", "No files specified");
        }
        else {
            stringstream ss(filenames_str);
            string filename;
            vector<string> files_to_upload;
            while (ss >> filename) {
                files_to_upload.push_back(filename);
            }
            ftp_mput(g_control_sockfd, files_to_upload);
        }
    }
    else if (input.substr(0, 5) == "mget ") {
        string filenames_str = input.substr(5);
        if (filenames_str.empty()) {
            cout << "Usage: mget <file1> <file2> ...\n";
            write_to_log("MGET", "N/A", "Failed", "No files specified");
        }
        else {
            stringstream ss(filenames_str);
            string filename;
            vector<string> files_to_download;
            while (ss >> filename) {
                files_to_download.push_back(filename);
            }
            ftp_mget(g_control_sockfd, files_to_download);
        }
    }
    else if (!input.empty()) {
        sendCommand(g_control_sockfd, input + "\r\n");
    }
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        cerr << "WSAStartup failed.\n";
        write_to_log("Startup", "N/A", "Failed", "WSAStartup error: " + to_string(WSAGetLastError()));
        return 1;
    }
    write_to_log("Startup", "N/A", "Success", "WSAStartup initialized");

    cout << "Enter FTP commands (type 'help' or '?' for commands, 'quit' or 'bye' to exit):\n";
    cout << "Use 'open <ip> [port]' to connect to an FTP server.\n";

    string input;
    while (true) {
        cout << "Client> ";
        getline(cin, input);
        if (input == "quit" || input == "bye") {
            handleFtpCommand(input);
            break;
        }
        handleFtpCommand(input);
    }

    closesocket(g_control_sockfd);
    WSACleanup();
    write_to_log("Shutdown", "N/A", "Success", "Program terminated");
    return 0;
}