#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>

using namespace std;
int main() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; //Allows connections from any IP address (e.g., localhost, external IPs).
    address.sin_port = htons(8080);

    bind(server_fd, (sockaddr*)&address, sizeof(address)); // binds the server to that specifc ip and port
// the server can listen for incoming connections on this address and port
    listen(server_fd, 1);
    cout << "Waiting for client...\n";

    int client_fd = accept(server_fd, nullptr, nullptr);
    
    
   
    // nullptr sockaddr structure where the client's address information (IP address and port) will be stored.
    // address length

    cout<<"Client connected.\n";

    fd_set readfds;
    char buffer[1024];

    while (true) {
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);  // Input
        FD_SET(client_fd, &readfds);     // to set the client for messages
        int maxfd = client_fd > STDIN_FILENO ? client_fd : STDIN_FILENO;  //The select function is used to check multiple file descriptors (like client_fd for the client socket and STDIN_FILENO for keyboard input) to see if they have data ready to read.

        select(maxfd + 1, &readfds, nullptr, nullptr, nullptr);
        
        //Ignores writability (writefds = nullptr).
	//Ignores errors (exceptfds = nullptr).
	//Waits forever (timeout = nullptr).
        
        
        
        
        //The FD_ISSET is used to check whether a specific file descriptor (like STDIN_FILENO or client_fd) has data ready to read

        // Check keyboard input
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            string msg;
            getline(cin, msg);
            send(client_fd, msg.c_str(), msg.length(), 0);
            if (msg == "exit") break;
        }






        // Check incoming message
        if (FD_ISSET(client_fd, &readfds)) {
            memset(buffer, 0, sizeof(buffer));
            int bytes = read(client_fd, buffer, sizeof(buffer) - 1);
            if (bytes <= 0) break;
            cout << "\nClient: " << buffer << "\nYou: " <<flush;
            if (strncmp(buffer, "exit", 4) == 0) break;
        }
    }

    close(client_fd);
    close(server_fd);
    cout << "Connection closed.\n";
    return 0;
}

