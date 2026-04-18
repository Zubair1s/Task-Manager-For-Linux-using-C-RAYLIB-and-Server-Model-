#include "raylib.h" 
#include <iostream>
#include <string>
#include <vector>

#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <array>
#include <iomanip>
#include <unistd.h>
#include <sys/statvfs.h>
#include <cerrno>
#include <signal.h>
#include <numeric>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>

using namespace std;

const int screenWidth = 1280;
const int screenHeight = 720;
const int borderPadding = 15;

Font font, boldfont;

// Menu options
enum MenuOption { 
    SYSTEM_SPECS,
    PROCESSES, 
    USAGE, 
    EXIT 
};

// Global variables for scrollbar dragging
bool draggingVerticalScroll = false;
Vector2 mousePrevPos = { 0 };

// Usage history for graphs
const int MAX_SAMPLES = 100;

array<float, MAX_SAMPLES> cpuUsageHistory = {0};
array<float, MAX_SAMPLES> memUsageHistory = {0};

int usageSampleIndex = 0;
float minCpuUsage = 100.0f, maxCpuUsage = 0.0f;
float minMemUsage = 100.0f, maxMemUsage = 0.0f;


// Moving average for avg CPU temperature
const int tempsmoothingsamples = 5;
array<float, tempsmoothingsamples> avgTempHistory = {0};
int avgTempIndex = 0;
int avgTempCount = 0;



// Cached usage data to reduce system calls
struct UsageCache {
    float cpuUsage = 0.0f;
    float memUsage = 0.0f;
    float diskUsage = 0.0f;
    float maxCpuTemp = 0.0f;
    float uptimeSeconds = 0.0f;
    double lastUpdateTime = 0.0;
};
UsageCache usageCache;


// Process info structure
struct ProcessInfo {
    string user;
    string pid;
    string command;
    string nlwp;
    string cpu;
    string mem;
};

// Client socket
int clientSock = -1;


// Function to execute shell command and get output
string ExecuteCommand(const char* cmd) {
    char buffer[256];
    string result;
    FILE* pipe = popen(cmd, "r");   //popen create a child process and transder and recive data using pipe

    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) { //  reading from a file or a pipe, once all the available data has been read, fgets() will return nullptr to indicate there’s no more data.
        result += buffer;
    }
    pclose(pipe);
    return result;
}




// Initialize client socket
bool InitializeClientSocket(const string& serverIP) {
    clientSock = socket(AF_INET, SOCK_STREAM, 0);  //AFINET address family, which in this case is IPv4 also stream socket + default protocol is used
    //file descriptor
    if (clientSock < 0) return false;

    sockaddr_in server_addr{};  
    server_addr.sin_family = AF_INET; //This specifies the address family (IPv4).
    server_addr.sin_port = htons(8080); // port for communication  host byte order to network byte order.ll data is expected to use big-endian order, also known as network byte order.

    if (inet_pton(AF_INET, serverIP.c_str(), &server_addr.sin_addr) <= 0) {
        cerr << "Invalid server address: " << serverIP << endl;
        close(clientSock);
        clientSock = -1;
        return false;
    }

    if (connect(clientSock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        cerr << "Connection to server failed: "<< endl;
        close(clientSock);
        clientSock = -1;
        return false;
    }

    cout << "Connected to server at " << serverIP << ":8080" << endl;
    return true;
}


// Send message to server
bool SendToServer(const string& message) {
    if (clientSock < 0) return false;

    if (send(clientSock, message.c_str(), message.length(), 0) < 0) {
        cerr << "Failed to send message to server: " << strerror(errno) << endl;
        close(clientSock);
        clientSock = -1;
        return false;
    }
    return true;
}



// Get average CPU temperature
float GetAvgCPUTemperature() {
    string result = ExecuteCommand("sensors -u");
    if (result.empty()) {// in case failed
        return 0.0f;
    }

    istringstream iss(result);
    string line;
    bool inCpuSection = false;
    vector<float> temperatures;

    while (getline(iss, line)) {
        if (line.find("coretemp-") != string::npos || line.find("k10temp-") != string::npos || line.find("cpu_thermal-") != string::npos) { //string::npos is returned if not found.
            inCpuSection = true;
        } 
        else if (line.find(":") == string::npos && !line.empty()) {
            inCpuSection = false;
        }

        if (inCpuSection && line.find("temp") != string::npos && line.find("_input") != string::npos) {
            try {
                size_t pos = line.find(":");
                if (pos != string::npos) {
                    string tempStr = line.substr(pos + 1);
                    float temp = stof(tempStr);
                    if (temp >= 0.0f && temp <= 150.0f) {
                        temperatures.push_back(temp);
                    }
                }
            } catch (...) {
                cerr << "Failed to parse avg temperature from line: " << line << endl;
            }
        }
    }

    if (temperatures.empty()) {
        cerr << "No valid CPU temperatures found for avg in sensors output" << endl;
        return 0.0f;
    }

    float avgTemp = accumulate(temperatures.begin(), temperatures.end(), 0.0f) / temperatures.size();

    avgTempHistory[avgTempIndex] = avgTemp;
    avgTempIndex = (avgTempIndex + 1) % tempsmoothingsamples;
    if (avgTempCount < tempsmoothingsamples) avgTempCount++;

    float sum = accumulate(avgTempHistory.begin(), avgTempHistory.begin() + avgTempCount, 0.0f);

    float smoothedTemp = (avgTempCount > 0) ? (sum / avgTempCount) : avgTemp;

    return smoothedTemp;
}



// Get number of CPU cores
int GetCPUCount() {
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? static_cast<int>(count) : 1;
}


vector<string> GetSystemSpecs() {  // Get system specifications
    string result = ExecuteCommand(
        "cat /etc/os-release | grep PRETTY_NAME && "
        "uname -r && "
        "lscpu | grep -E 'Model name|Architecture|CPU\\(s\\)' && "
        "free -h | grep Mem && "
        "df -h | grep -E '^/dev/'"
    );

    vector<string> lines;
    istringstream stream(result);
    string line;
    while (getline(stream, line)) {
        lines.push_back(line);
    }
    return lines;
}

float GetDiskUsage() {   // Get disk usage
    struct statvfs stat;
    statvfs("/", &stat);  // alternative  df -h to get result but this is low level and fast
    unsigned long total = stat.f_blocks * stat.f_bsize;
    unsigned long available = stat.f_bavail * stat.f_bsize;
    unsigned long used = total - available;
    return total == 0 ? 0.0f : ((float)used / total) * 100.0f;
}


float GetMemoryUsage() {   // Get memory usage  Reads directly from kernel-provided stats and more efficient.
    FILE* fp = fopen("/proc/meminfo", "r");
    if (!fp) {
        return 0.0f;
    }
    long total = 1, free = 0, buffers = 0, cached = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "MemTotal:", 9) == 0) sscanf(line + 9, "%ld", &total); // line memtotal is compared with line which has the 256 characters ouput
        else if (strncmp(line, "MemFree:", 8) == 0) sscanf(line + 8, "%ld", &free); // and when true returns 0 and then line + 9 is extracted in total 
        else if (strncmp(line, "Buffers:", 8) == 0) sscanf(line + 8, "%ld", &buffers); // with given datatype
        else if (strncmp(line, "Cached:", 7) == 0) sscanf(line + 7, "%ld", &cached);
    }
    fclose(fp);
    if (total == 0) return 0.0f;
    long used = total - free - buffers - cached;
    return ((float)used / total) * 100.0f;
}

//taking two snapshots for the cpu like car meter reading never goes backwards
float GetCPUUsage() {  // Get CPU usage
    static long prevIdle = 0, prevTotal = 0;
    FILE* fp = fopen("/proc/stat", "r");
    if (!fp) {
        return 0.0f;
    }
// getting all the cpu time from the file into varibles for the calculation
    long user, nice, system, idle, iowait, irq, softirq, steal; 
    if (fscanf(fp, "cpu %ld %ld %ld %ld %ld %ld %ld %ld",&user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) != 8) {
        //how much amount of time cpu has spent on various states since the system booted in jiffies 
        fclose(fp);
// Nicing is a way to voluntarily lower the priority of a process so it gets fewer CPU resources than other user-level processes.
// irq (Interrupt Request): 
//steal value is specific to virtualized environments  CPU has spent waiting for the real CPU while another virtual machine was running on the same physical hardware.
        return 0.0f;
    }
    fclose(fp);

    long idleTime = idle + iowait;
    long nonIdle = user + nice + system + irq + softirq + steal;
    long total = idleTime + nonIdle;

    float usage = 0.0f;
    if (prevTotal != 0) {
        long totalDiff = total - prevTotal;
        long idleDiff = idleTime - prevIdle;
        usage = (totalDiff == 0) ? 0.0f : ((float)(totalDiff - idleDiff) / totalDiff) * 100.0f;
    }

    prevTotal = total;
    prevIdle = idleTime;
    return usage;
}

float getuptime(){
    string uptimeResult = ExecuteCommand("cat /proc/uptime");
    float uptime = 0.0f;
    if (!uptimeResult.empty()) {
        try {
            istringstream iss(uptimeResult); 
            iss >> uptime;  //>>) by default reads data from the input stream, skipping any leading whitespace (spaces, tabs, newlines)
    }   catch (...) {
            uptime = 0.0f;
    }
}
return uptime;
}

// Get system usage data
void GetUsageData(float* cpuUsage, float* memUsage, float* diskUsage, float* maxCpuTemp, float* uptimeSeconds) {
    double currentTime = GetTime();

    if (currentTime - usageCache.lastUpdateTime < 2.0) {
        *cpuUsage = usageCache.cpuUsage;
        *memUsage = usageCache.memUsage;
        *diskUsage = usageCache.diskUsage;
        *maxCpuTemp = usageCache.maxCpuTemp;
        *uptimeSeconds = usageCache.uptimeSeconds;
        return;
    }

    *cpuUsage = GetCPUUsage();
    *memUsage = GetMemoryUsage();
    *diskUsage = GetDiskUsage();
    *maxCpuTemp = GetAvgCPUTemperature();
    float uptime = getuptime();

    *uptimeSeconds = uptime;

    usageCache.cpuUsage = *cpuUsage;
    usageCache.memUsage = *memUsage;
    usageCache.diskUsage = *diskUsage;
    usageCache.maxCpuTemp = *maxCpuTemp;
    usageCache.uptimeSeconds = *uptimeSeconds;
    usageCache.lastUpdateTime = currentTime;

    minCpuUsage = min(minCpuUsage, *cpuUsage);
    maxCpuUsage = max(maxCpuUsage, *cpuUsage);
    minMemUsage = min(minMemUsage, *memUsage);
    maxMemUsage = max(maxMemUsage, *memUsage);

    if (*maxCpuTemp > 80.0f) {
        string msg = "PC 1 High CPU Temp: " + to_string(int(*maxCpuTemp)) + "°C";
        SendToServer(msg);
    }
}



// fectching running processs in processinfo vector every 2 secs
vector<ProcessInfo> GetRunningProcesses() {
    string result = ExecuteCommand("ps -eo user,pid,comm,nlwp,%cpu,%mem --no-headers --sort=-%mem");
    vector<ProcessInfo> processes;

    istringstream iss(result);
    string line;

    if (result.empty()) {
        cerr << "No output from ps command" << endl;
        return processes;
    }

    while (getline(iss, line)) {
        if (line.empty()) continue;

        ProcessInfo info;
        istringstream lineStream(line);
        vector<string> tokens;

        string token;
        while (lineStream >> token) {
            tokens.push_back(token);
        }

        if (tokens.size() < 5) {
            cerr << "Failed to parse line: " << line << endl;
            continue;
        }

        info.user = tokens[0];
        info.pid = tokens[1];
        info.nlwp = tokens[tokens.size() - 3];
        info.cpu = tokens[tokens.size() - 2]; // for safer executeing if the process name is long 
        info.mem = tokens[tokens.size() - 1];

        info.command = "";
        for (size_t i = 2; i < tokens.size() - 3; ++i) {
            info.command += tokens[i];
            if (i < tokens.size() - 4) info.command += " ";
        }

        try {
            replace(info.cpu.begin(), info.cpu.end(), ',', '.');
            replace(info.mem.begin(), info.mem.end(), ',', '.');

            float cpuVal = stof(info.cpu);
            float memVal = stof(info.mem);
            
            int cpuCount = GetCPUCount();

            if (cpuCount > 1) {
                cpuVal /= cpuCount;
            }

            if (cpuVal < 0.0f || cpuVal > 100.0f) {
                info.cpu = "0.0";

            } else {
                char cpuStr[16];
                snprintf(cpuStr, sizeof(cpuStr), "%.1f", cpuVal);
                info.cpu = cpuStr;
            }
            if (memVal < 0.0f || memVal > 100.0f) {
                info.mem = "0.0";
            }
            // Check process CPU usage threshold
            if (cpuVal > 30.0f) {
                string msg = "High CPU Usage: PID " + info.pid + ", " + to_string(static_cast<int>(cpuVal)) + "%";
                SendToServer(msg);
            }
        } catch (const exception& e) {
            cerr << "Invalid CPU/MEM format in line: " << line << endl;
            info.cpu = "0.0";
            info.mem = "0.0";
        }

        try {
            int nlwpVal = stoi(info.nlwp);
            if (nlwpVal < 0) {
                info.nlwp = "0";
            }
        } catch (...) {
            info.nlwp = "0";
        }
        

        processes.push_back(info);
    }

    cout << "Fetched " << processes.size() << "processes." << endl;
    return processes;
}





// Kill process by PID
bool KillProcess(int pid, string& errorMsg) {
    if (pid <= 0 || pid > 4194304) {
        errorMsg = "Invalid PID: Must be between 1 and 4194304.";
        return false;
    }
//Signal 0 is a special signal in the kill command, and it's used for checking the existence of a process without actually killing it.
    if (kill(pid, 0) != 0) {
        errorMsg = "Process with PID " + to_string(pid) + " not found: ";
        return false;
    }

    if (kill(pid, SIGTERM) != 0) {
        errorMsg = "Failed to send SIGTERM to PID " + to_string(pid);
        return false;
    }

    usleep(200000); //20ms

    if (kill(pid, 0) == 0) {
        if (kill(pid, SIGKILL) != 0) {
            errorMsg = "Failed to send SIGKILL to PID " + to_string(pid);
            return false;
        }

        usleep(200000);

        if (kill(pid, 0) == 0) {
            errorMsg = "Process with PID " + to_string(pid) + " could not be terminated: Possibly a protected system process.";
            return false;
        }
    }

    errorMsg = "Process with PID " + to_string(pid) + " killed successfully.";
    return true;
}

// Process server commands
void ProcessServerCommands(bool& processesNeedRefresh, int& selectedProcessIndex, string& killErrorMsg, double& killMsgDisplayTime) {
    if (clientSock < 0) return;

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(clientSock, &readfds);
    struct timeval tv = {0, 0}; // Non-blocking

    if (select(clientSock + 1, &readfds, nullptr, nullptr, &tv) > 0) { //select is to check for data insocket
        char buffer[1024] = {0};
        int bytes = read(clientSock, buffer, sizeof(buffer) - 1); 
        if (bytes <= 0) {
            cerr << "Server disconnected: "<< endl;
            close(clientSock);
            clientSock = -1;
            return;
        }

        string command(buffer);
        cout << "Received from server: " << command << endl;

        if (command == "shutdown") {
            ExecuteCommand("sudo shutdown -h now");
        } 
        else if (command.substr(0, 5) == "kill ") 
        {   try {
                int pid = stoi(command.substr(5));
                killErrorMsg.clear();
                if (KillProcess(pid, killErrorMsg)) {
                    processesNeedRefresh = true;
                    selectedProcessIndex = -1;
                    killMsgDisplayTime = GetTime();
                }
            } catch (...) {
                killErrorMsg = "Invalid kill command format: " + command;
                killMsgDisplayTime = GetTime();
            }
        }
    }
}



// Draw process list on the content area
void DrawProcessList(const vector<ProcessInfo>& processes, Rectangle bounds, float* scrollY, int* selectedProcessIndex, bool isDarkMode) {
    float fontSize = 20, spacing = 2;
    Color tint = isDarkMode ? WHITE : BLACK;

    float lineHeight = fontSize + spacing;
    float contentHeight = processes.size() * lineHeight;
    
    const int colWidths[] = {120, 80, 300, 100, 100, 100};
    const char* headers[] = {"USER", "PID", "COMMAND", "NLWP", "%CPU", "%MEM"};
    
    float maxLineWidth = 0;
    for (int i = 0; i < 6; i++) maxLineWidth += colWidths[i];
    
    Rectangle verticalScrollBar = {0};
    Rectangle verticalScrollThumb = {0};
    
    if (contentHeight > bounds.height) {
        float scrollbarWidth = 8;
        float scrollbarHeight = bounds.height * (bounds.height / contentHeight);
        
        scrollbarHeight = max(20.0f, min(scrollbarHeight, bounds.height));
        
        float scrollbarY = bounds.y + (*scrollY / contentHeight) * bounds.height;
        
        verticalScrollBar = {bounds.x + bounds.width - scrollbarWidth, bounds.y, scrollbarWidth, bounds.height};
        verticalScrollThumb = {bounds.x + bounds.width - scrollbarWidth, scrollbarY, scrollbarWidth, scrollbarHeight};
        
        DrawRectangleRounded(verticalScrollBar, 0.5f, 8, isDarkMode ? Color{70, 70, 70, 255} : Color{200, 200, 200, 255});
        DrawRectangleRounded(verticalScrollThumb, 0.5f, 8, isDarkMode ? Color{100, 100, 100, 255} : Color{100, 100, 100, 255});
    }

    Vector2 mousePos = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (CheckCollisionPointRec(mousePos, verticalScrollThumb)) {
            draggingVerticalScroll = true;
            mousePrevPos = mousePos;
        }
    }
    
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) {
        draggingVerticalScroll = false;
    }
    
    if (draggingVerticalScroll) {
        float deltaY = mousePos.y - mousePrevPos.y;
        *scrollY += deltaY * (contentHeight / bounds.height);
        *scrollY = max(0.0f, min(*scrollY, contentHeight - bounds.height));
        mousePrevPos = mousePos;
    }
    

    BeginScissorMode(int(bounds.x), int(bounds.y), int(bounds.width - (contentHeight > bounds.height ? 8 : 0)), int(bounds.height));

    float x = bounds.x;
    float y = bounds.y - *scrollY;
    
    //drawing the headings for the process list
    for (int i = 0; i < 6; i++) {
        if (y + lineHeight > bounds.y && y < bounds.y + bounds.height) {
            DrawTextEx(boldfont, headers[i], {x, y}, fontSize, spacing, tint);
        }
        x += colWidths[i];
    }

    y += lineHeight;

    int index = 0;
    for (const auto& proc : processes) {
        Rectangle rowRect = {bounds.x, y, maxLineWidth, lineHeight};
        
        bool hovered = CheckCollisionPointRec(mousePos, {bounds.x, y, bounds.width - (contentHeight > bounds.height ? 8 : 0), lineHeight});
        
        if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            *selectedProcessIndex = index;
        }
        
        if (y + lineHeight > bounds.y && y < bounds.y + bounds.height) {
            x = bounds.x;
            
            if (index == *selectedProcessIndex) {
                DrawRectangleLinesEx({bounds.x, y, maxLineWidth, lineHeight}, 2, {66, 135, 245, 255});
            }
            
            DrawTextEx(font, TextSubtext(proc.user.c_str(), 0, colWidths[0] / 4), {x, y}, fontSize, spacing, tint); x += colWidths[0];
            DrawTextEx(font, TextSubtext(proc.pid.c_str(), 0, colWidths[1] / 4), {x, y}, fontSize, spacing, tint); x += colWidths[1];
            DrawTextEx(font, TextSubtext(proc.command.c_str(), 0, colWidths[2] / 4), {x, y}, fontSize, spacing, tint); x += colWidths[2];
            DrawTextEx(font, TextSubtext(proc.nlwp.c_str(), 0, colWidths[3] / 4), {x, y}, fontSize, spacing, tint); x += colWidths[3];

            char cpuStr[16], memStr[16];
            try {
                snprintf(cpuStr, sizeof(cpuStr), "%.1f", stof(proc.cpu));
                snprintf(memStr, sizeof(cpuStr), "%.1f", stof(proc.mem));
            } catch (...) {
                strcpy(cpuStr, "0.0");
                strcpy(memStr, "0.0");
            }
            DrawTextEx(font, cpuStr, {x, y}, fontSize, spacing, tint); x += colWidths[4];
            DrawTextEx(font, memStr, {x, y}, fontSize, spacing, tint);
        }
        y += lineHeight;
        index++;

    }
    EndScissorMode();
}


// end task button making this shit modern 
bool endtaskButton(Rectangle bounds, bool enabled, bool isDarkMode, Color baseColor = {200, 200, 200, 255}) {
    bool clicked = false;
    Vector2 mousePos = GetMousePosition();
    bool hovered = enabled && CheckCollisionPointRec(mousePos, bounds);

    Color buttonColor = enabled ? (isDarkMode ? Color{100, 100, 100, 255} : baseColor) : (isDarkMode ? Color{70, 70, 70, 255} : Color{150, 150, 150, 255});
    
    if (hovered) buttonColor = GRAY;
    
    DrawRectangleRounded(bounds, 0.3f, 8, buttonColor);
    // remove this shit
    DrawRectangleRoundedLines(bounds, 0.3f, 8, isDarkMode ? Color{150, 150, 150, 255} : Color{100, 100, 100, 255});
    DrawTextEx(font, "End Task", {bounds.x + bounds.width/2 - 35, bounds.y + bounds.height/2 - 8}, 20, 2, isDarkMode ? Color{200, 200, 200, 255} : (enabled ? BLACK : GRAY));
    
    if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        clicked = true;
    }
    
    return clicked;
}

// this is refresh button 
bool refreshbutton(Rectangle bounds, Texture2D icon, bool isDarkMode) {
    bool clicked = false;
    Color baseColor = {200, 200, 200, 255};

    Vector2 mousePos = GetMousePosition();
    bool hovered = CheckCollisionPointRec(mousePos, bounds);
    
    Color buttonColor = isDarkMode ? Color{100, 100, 100, 255} : baseColor;
    if (hovered) buttonColor = GRAY;
    
    DrawRectangleRounded(bounds, 0.3f, 8, buttonColor);
    DrawRectangleRoundedLines(bounds, 0.3f, 8, isDarkMode ? Color{150, 150, 150, 255} : Color{100, 100, 100, 255});
    
    Rectangle iconSrc = {0, 0, static_cast<float>(icon.width), static_cast<float>(icon.height)};
    DrawTexturePro(icon, iconSrc, {bounds.x + bounds.width/2 - 16, bounds.y + bounds.height/2 - 16, 32, 32}, {0, 0}, 0, WHITE);
    if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        clicked = true;
    }
    
    return clicked;
}

// Draw pie chart
void DrawPieChart(Rectangle bounds, float diskUsage, bool isDarkMode) {
    Vector2 center = {bounds.x + bounds.width/2, bounds.y + bounds.height/2};

    float radius = bounds.width/2;
    float startAngle = 0;
    float usedAngle = (diskUsage / 100.0f) * 360.0f;
    
    DrawCircleSector(center, radius, startAngle, startAngle + usedAngle, 64, {156, 39, 176, 255});

    DrawCircleSector(center, radius, startAngle + usedAngle, 360, 64, isDarkMode ? Color{70, 70, 70, 255} : Color{200, 200, 200, 255});
    
    DrawCircleSectorLines(center, radius, 0, 360, 64, isDarkMode ? Color{150, 150, 150, 255} : Color{100, 100, 100, 255});
    
    string diskStats = "Disk Usage:\nUsed: " + to_string(int(diskUsage)) + "%\nFree: " + to_string(int(100.0f - diskUsage)) + "%";
    DrawTextEx(font, diskStats.c_str(), {bounds.x, bounds.y - 80}, 18, 2, isDarkMode ? Color{200, 200, 200, 255} : BLACK);
}



// draw all the usage graphs
void DrawUsageGraphs(Rectangle bounds, float diskUsage, bool isDarkMode, float maxCpuTemp, float uptimeSeconds) {
    
    float currentCpu = cpuUsageHistory[(usageSampleIndex - 1 + MAX_SAMPLES) % MAX_SAMPLES];
    float currentMem = memUsageHistory[(usageSampleIndex - 1 + MAX_SAMPLES) % MAX_SAMPLES];
    

    //drawing the memory graph
    Rectangle cpuGraphBounds = {bounds.x + 30, bounds.y + 120, (bounds.width - 100) / 2, 150};  // place where to draw the graph
    DrawRectangleRounded(cpuGraphBounds, 0.2f, 8, isDarkMode ? Color{0, 0, 0, 255} : Color{230, 230, 230, 255});
    

    //adding grid lines for graph
    for (int i = 1; i <= 4; i++) {
        float y = cpuGraphBounds.y + (cpuGraphBounds.height * i / 5);
        DrawLine(int(cpuGraphBounds.x), int(y),int(cpuGraphBounds.x + cpuGraphBounds.width), int(y), isDarkMode ? Color{100, 100, 100, 100} : Color{200, 200, 200, 100});
    }

    
    //drawing the graph
    float graphWidth = cpuGraphBounds.width / (MAX_SAMPLES - 1);

    for (int i = 1; i < MAX_SAMPLES; i++) {
        float x1 = cpuGraphBounds.x + (i - 1) * graphWidth;
        float x2 = cpuGraphBounds.x + i * graphWidth;

        float y1 = cpuGraphBounds.y + cpuGraphBounds.height - (cpuUsageHistory[(usageSampleIndex + i - 1) % MAX_SAMPLES] / 100.0f) * cpuGraphBounds.height;
        float y2 = cpuGraphBounds.y + cpuGraphBounds.height - (cpuUsageHistory[(usageSampleIndex + i) % MAX_SAMPLES] / 100.0f) * cpuGraphBounds.height;
        
        DrawLineEx({x1, y1}, {x2, y2}, 2, {66, 135, 245, 255});
    }
    
    string cpuStats = "CPU Usage:\nCurrent: " + to_string(static_cast<int>(currentCpu)) + "%\nMin: " + to_string(static_cast<int>(minCpuUsage)) + "%\nMax: " + to_string(static_cast<int>(maxCpuUsage)) + "%";
    DrawTextEx(font, cpuStats.c_str(), {cpuGraphBounds.x, cpuGraphBounds.y - 80}, 18, 2, isDarkMode ? Color{200, 200, 200, 255} : BLACK);
    

    //drawing the memory graph 


    Rectangle memGraphBounds = {bounds.x + 30, cpuGraphBounds.y + cpuGraphBounds.height + 100, (bounds.width - 100) / 2, 150};
    DrawRectangleRounded(memGraphBounds, 0.2f, 8, isDarkMode ? Color{0, 0, 0, 255} : Color{230, 230, 230, 255});
    
    for (int i = 1; i <= 4; i++) {
        float y = memGraphBounds.y + (memGraphBounds.height * i / 5);
        DrawLine(int(memGraphBounds.x), int(y),int(memGraphBounds.x + memGraphBounds.width), int(y), isDarkMode ? Color{100, 100, 100, 100} : Color{200, 200, 200, 100});
    }
    for (int i = 1; i < MAX_SAMPLES; i++) {
        float x1 = memGraphBounds.x + (i-1) * graphWidth;
        float x2 = memGraphBounds.x + i * graphWidth;
        float y1 = memGraphBounds.y + memGraphBounds.height - (memUsageHistory[(usageSampleIndex + i - 1) % MAX_SAMPLES] / 100.0f) * memGraphBounds.height;
        float y2 = memGraphBounds.y + memGraphBounds.height - (memUsageHistory[(usageSampleIndex + i) % MAX_SAMPLES] / 100.0f) * memGraphBounds.height;
        DrawLineEx({x1, y1}, {x2, y2}, 2, {76, 175, 80, 255});
    }
    
    string memStats = "Memory Usage:\nCurrent: " + to_string(int(currentMem)) + "%\nMin: " + to_string(int(minMemUsage)) + "%\nMax: " + to_string(int(maxMemUsage)) + "%";
    DrawTextEx(font, memStats.c_str(), {memGraphBounds.x, memGraphBounds.y - 80}, 18, 2, isDarkMode ? Color{200, 200, 200, 255} : BLACK);
    
    
    //drawing the disk usage
    Rectangle diskPieBounds = {bounds.x + bounds.width - 300, bounds.y + 120, 200, 200};
    DrawPieChart(diskPieBounds, diskUsage, isDarkMode);


    //drawing average temps
    Color avgTempColor = (maxCpuTemp > 85.0f) ? RED : (isDarkMode ? Color{200, 200, 200, 255} : BLACK);
    string avgTempText = "Avg CPU Temperature: " + to_string(int(maxCpuTemp)) + "°C";
    DrawTextEx(font, avgTempText.c_str(), {bounds.x + 30, memGraphBounds.y + memGraphBounds.height + 60}, 18, 2, avgTempColor);


    //calculating the time spend on 
    int totalSeconds = int(uptimeSeconds);
    int hours = totalSeconds / 3600;
    totalSeconds %= 3600;
    int minutes = totalSeconds / 60;
    int seconds = totalSeconds % 60;

    string uptimeText = "Uptime: " + to_string(hours) + "h " + to_string(minutes) + "m " + to_string(seconds) + "s";
    DrawTextEx(font, uptimeText.c_str(), {bounds.x + 30, memGraphBounds.y + memGraphBounds.height + 90}, 18, 2, isDarkMode ? Color{200, 200, 200, 255} : BLACK);
}




// draw main content area
void DrawContentArea(MenuOption selectedMenu, vector<string>& specsLines, vector<ProcessInfo>& processes, 
                    float& processScrollY, int& pidToKill, int& selectedProcessIndex,
                    bool& killResult, string& killErrorMsg, Texture2D refreshIcon, 
                    bool& processesNeedRefresh, float diskUsage, double& killMsgDisplayTime, 
                    bool isDarkMode, float currentTemp, float maxCpuTemp, float uptimeSeconds) {

    Rectangle contentArea = {
        borderPadding + 80 + borderPadding,
        float(borderPadding),
        float(screenWidth - 80 - 3*borderPadding), // drawing the content area on screen
        float(screenHeight - 2*borderPadding)
    };
    
    DrawRectangleRounded(contentArea, 0.1f, 8, isDarkMode ? BLACK : WHITE); // for roundness
    
    // system specification page
    if (selectedMenu == SYSTEM_SPECS) {
        DrawTextEx(boldfont, "System Specifications", {contentArea.x + 460, contentArea.y + 20}, 32, 2, isDarkMode ? WHITE : BLACK); 
        
        float y = contentArea.y + 60;
        float fontSize = 20;
        float spacing = 2;
        float lineHeight = fontSize + spacing;
        
        BeginScissorMode(int(contentArea.x + 20), int(contentArea.y + 60), int(contentArea.width - 40), int(contentArea.height - 80));
        for (const auto& line : specsLines) {
            
            if (y + lineHeight > contentArea.y + 60 && y < contentArea.y + contentArea.height - 20) {
                DrawTextEx(font, line.c_str(), {contentArea.x + 20, y}, fontSize, spacing, isDarkMode ? WHITE : BLACK);
            }
            y += lineHeight;
        }
        EndScissorMode();

    }

    // process page 
    else if (selectedMenu == PROCESSES) {
        
        DrawTextEx(boldfont, "Running Processes", {contentArea.x + 460, contentArea.y + 20}, 32, 2, isDarkMode ? Color{200, 200, 200, 255} : BLACK);
        
        Rectangle refreshButtonRect = {contentArea.x + 20 ,contentArea.y + 60 ,40 ,40}; 
        Rectangle endTaskButtonRect = {refreshButtonRect.x + refreshButtonRect.width + 10, contentArea.y + 60, 100, 40};
        

        Rectangle processListBounds = {
            contentArea.x + 20,
            contentArea.y + 120,
            contentArea.width - 40,
            contentArea.height - 140
        };
        
        
        bool processSelected = selectedProcessIndex >= 0 && selectedProcessIndex < static_cast<int>(processes.size());

        
        if (processSelected) {
            int pid = std::stoi(processes[selectedProcessIndex].pid);
            
            if (pid <= 100) {
                DrawTextEx(boldfont, "Warning: Low PID may be a system process!", {endTaskButtonRect.x + 120, endTaskButtonRect.y + (endTaskButtonRect.height)/2}, 16, 2, RED);
            }
            
            if (endtaskButton(endTaskButtonRect, true, isDarkMode)) {
                pidToKill = pid;  //save the pid for killing
                killErrorMsg.clear(); // clear old shitty messages 
                killResult = KillProcess(pidToKill, killErrorMsg); // send pid for killing to killprocess function
                processesNeedRefresh = true;
                
                selectedProcessIndex = -1;
                killMsgDisplayTime = GetTime();
            }

        } 
        else {
            endtaskButton(endTaskButtonRect, false, isDarkMode);
        }
        

        if (refreshbutton(refreshButtonRect, refreshIcon, isDarkMode)) {
            processesNeedRefresh = true;
            selectedProcessIndex = -1;
            killErrorMsg.clear();
        }
        
        if (!killErrorMsg.empty()) {
            DrawTextEx(font, killErrorMsg.c_str(), {endTaskButtonRect.x + 130, endTaskButtonRect.y}, 20, 2, killResult ? GREEN : RED);
        }
        
        DrawProcessList(processes, processListBounds, &processScrollY, &selectedProcessIndex, isDarkMode);
    }


    // usage page in contentarea
    else if (selectedMenu == USAGE) {
        
        DrawTextEx(boldfont, "System Usage", {contentArea.x + 460, contentArea.y + 20}, 32, 2, isDarkMode ? Color{200, 200, 200, 255} : BLACK);
        DrawUsageGraphs(contentArea, diskUsage, isDarkMode, maxCpuTemp, uptimeSeconds);
    }
}

// Update scroll positions
void UpdateScroll(MenuOption selectedMenu, float& processScrollY, vector<ProcessInfo>& processes, int menuWidth) {
    float wheel = GetMouseWheelMove();
    
    if (wheel != 0 && !draggingVerticalScroll) {
        if (selectedMenu == PROCESSES) {
            processScrollY -= wheel * 40;
            float maxScrollY = max(0.0f, static_cast<float>(processes.size()) * 24 - (screenHeight - 140));
            processScrollY = max(0.0f, min(processScrollY, maxScrollY));
        }
    }
}

// Update processes
void UpdateProcesses(MenuOption selectedMenu, vector<ProcessInfo>& processes, bool& processesNeedRefresh, double& lastProcessRefreshTime, string& killErrorMsg, double& killMsgDisplayTime) {
    double currentTime = GetTime();

    if (selectedMenu == PROCESSES && (processesNeedRefresh || currentTime - lastProcessRefreshTime > 2.0)) {
        processes = GetRunningProcesses();
        processesNeedRefresh = false;
        lastProcessRefreshTime = currentTime;
    }

    if (!killErrorMsg.empty() && currentTime - killMsgDisplayTime >= 2.0) {
        killErrorMsg.clear();
    }
}

// Update usage data
void UpdateUsageData(MenuOption selectedMenu, int& usageUpdateCounter, float& diskUsage, float& maxCpuTemp, float& uptimeSeconds) {
    if (selectedMenu == USAGE) {
        usageUpdateCounter++;
        
        if (usageUpdateCounter >= 60) {
            float cpuUsage, memUsage, maxTemp, uptime; // temp variables

            GetUsageData(&cpuUsage, &memUsage, &diskUsage, &maxTemp, &uptime);
            
            cpuUsageHistory[usageSampleIndex] = cpuUsage;
            memUsageHistory[usageSampleIndex] = memUsage;

            usageSampleIndex = (usageSampleIndex + 1) % MAX_SAMPLES;
            
            usageUpdateCounter = 0;
            
            maxCpuTemp = maxTemp;
            uptimeSeconds = uptime;
        }
    }
}



// Draw menu
void DrawMenu(MenuOption& selectedMenu, Texture2D* menuIcons, bool& processesNeedRefresh, bool& isDarkMode) {
    
    const int menuWidth = 80;
    Rectangle menuRect = {   //menu rectangle 
        (float)borderPadding,
        (float)borderPadding,
        (float)menuWidth,
        (float)(screenHeight - 2 * borderPadding)
    };
    
    DrawRectangleRounded(menuRect, 1, 8, isDarkMode ? Color{128, 128, 128, 255} : Color{255, 241, 228, 255}); // for roundnesssssss
    for (int i = 0; i < 5; i++) {
        
        Rectangle iconRect = {menuRect.x + menuWidth/2 - 24,     menuRect.y + 80 + float(i * 80)    , 50, 50};
        
        bool hovered = CheckCollisionPointRec(GetMousePosition(), iconRect);
        if (hovered || (i < 4 && selectedMenu == static_cast<MenuOption>(i))) 
        {            
            DrawRectangleLinesEx(iconRect, 2, {66, 135, 245, 255});   
        }
        Rectangle iconSrc = {0, 0, float(menuIcons[i].width), float(menuIcons[i].height)};  //add width height in simple
        DrawTexturePro(menuIcons[i], iconSrc, iconRect, {0, 0}, 0, WHITE);
        
        if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (i == 4) {
                isDarkMode = !isDarkMode;
            } else {
                selectedMenu = static_cast<MenuOption>(i);

                if (selectedMenu == PROCESSES) processesNeedRefresh = true;
                if (selectedMenu == EXIT) CloseWindow();
            }
        }
    }
}


// Initialize resources
void InitializeResources(Texture2D& specsIcon, Texture2D& processIcon, Texture2D& usageIcon, Texture2D& exitIcon, Texture2D& refreshIcon, Texture2D& darkModeIcon) {
    
    font = LoadFontEx("Poppins-Regular.ttf", 32, 0, 250);
    boldfont = LoadFontEx("Poppins-SemiBold.ttf", 32, 0, 250);
    

    SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(boldfont.texture, TEXTURE_FILTER_BILINEAR);    
    
    specsIcon = LoadTexture("images/specs.png");
    processIcon = LoadTexture("images/process.png");
    usageIcon = LoadTexture("images/usage.png");
    exitIcon = LoadTexture("images/exit.png");
    
    refreshIcon = LoadTexture("images/refresh.png");
    darkModeIcon = LoadTexture("images/darkmode.png");
;

    SetTextureFilter(specsIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(processIcon, TEXTURE_FILTER_BILINEAR);  // to improve the quality of images
    SetTextureFilter(usageIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(exitIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(refreshIcon, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(darkModeIcon, TEXTURE_FILTER_BILINEAR);
}

// Cleanup resources
void CleanupResources(Texture2D specsIcon, Texture2D processIcon, Texture2D usageIcon, Texture2D exitIcon, Texture2D refreshIcon, Texture2D darkModeIcon) {
    UnloadTexture(specsIcon);
    UnloadTexture(processIcon);
    UnloadTexture(usageIcon);
    UnloadTexture(exitIcon);
    UnloadTexture(refreshIcon);
    UnloadTexture(darkModeIcon);
    UnloadFont(font);
    UnloadFont(boldfont);
    
    if (clientSock >= 0) close(clientSock);
    CloseWindow();
}

int main() {
    SetConfigFlags(FLAG_WINDOW_HIGHDPI | FLAG_MSAA_4X_HINT);
    
    InitWindow(screenWidth, screenHeight, "Linux Task Manager");
    SetTargetFPS(60);
    
    
    // Prompt for server IP
    string serverIP;
    cout << "Enter server IP: ";
    getline(cin, serverIP);

    // Initialize client socket
    if (!InitializeClientSocket(serverIP)) {
        cerr << "Failed to connect to server. Continuing without server communication." << endl;
    }

    Texture2D specsIcon, processIcon, usageIcon, exitIcon, refreshIcon, darkModeIcon;

    InitializeResources(specsIcon, processIcon, usageIcon, exitIcon, refreshIcon, darkModeIcon);

    Texture2D menuIcons[] = {specsIcon, processIcon, usageIcon, exitIcon, darkModeIcon};

    MenuOption selectedMenu = SYSTEM_SPECS;
    vector<ProcessInfo> processes;

    float processScrollY = 0;
    bool processesNeedRefresh = true;
    double lastProcessRefreshTime = 0.0;
    int pidToKill = 0;
    int selectedProcessIndex = -1;
    bool killResult = false;
    string killErrorMsg = "";
    
    double killMsgDisplayTime = 0.0;
    
    vector<string> specsLines = GetSystemSpecs();
    
    int usageUpdateCounter = 0;
    
    float diskUsage = 0.0f;
    
    float currentTemp = 0.0f;
    
    float maxCpuTemp = 0.0f;
    
    float uptimeSeconds = 0.0f;
    
    bool isDarkMode = false;

    while (!WindowShouldClose()) {

        UpdateScroll(selectedMenu, processScrollY, processes, 80);
        UpdateProcesses(selectedMenu, processes, processesNeedRefresh, lastProcessRefreshTime, killErrorMsg, killMsgDisplayTime);
        UpdateUsageData(selectedMenu, usageUpdateCounter, diskUsage, maxCpuTemp, uptimeSeconds);
        ProcessServerCommands(processesNeedRefresh, selectedProcessIndex, killErrorMsg, killMsgDisplayTime);

        BeginDrawing();

        ClearBackground(isDarkMode ? Color{0, 0, 0, 255} : WHITE);  // check this 1
        
        DrawMenu(selectedMenu, menuIcons, processesNeedRefresh, isDarkMode);
        
        DrawContentArea(selectedMenu, specsLines, processes, processScrollY, pidToKill, selectedProcessIndex, 
                        killResult, killErrorMsg, refreshIcon, processesNeedRefresh, diskUsage, killMsgDisplayTime, 
                        isDarkMode, currentTemp, maxCpuTemp, uptimeSeconds);
        
        EndDrawing();
    }

    CleanupResources(specsIcon, processIcon, usageIcon, exitIcon, refreshIcon, darkModeIcon);

}