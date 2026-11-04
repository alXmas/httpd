#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define PORT 8080
#define BUFFER_SIZE 1024

int main() {
    int server_fd, client_fd;
    struct sockaddr_in server_addr;
    char buffer[BUFFER_SIZE];
    
    // 1. Create a TCP socket (AF_INET = IPv4, SOCK_STREAM = TCP)
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    // 2. Configure the server address structure
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY; // Listen on all network interfaces
    server_addr.sin_port = htons(PORT);       // Convert port to network byte order

    // 3. Bind the socket to the port
    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // 4. Start listening for incoming connections (backlog queue size = 10)
    if (listen(server_fd, 10) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    printf("HTTP Server running on http://localhost:%d\n", PORT);

    // 5. Infinite loop to accept and handle client requests
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        // Accept incoming client connection
        client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("Accept failed");
            continue;
        }

        // Read the incoming HTTP request from the client
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t bytes_read = read(client_fd, buffer, BUFFER_SIZE - 1);
        if (bytes_read > 0) {
            // Optional: Print raw request to the console
            printf("--- Received Request ---\n%s\n", buffer);
        }

        // 6. Build the raw HTTP response string
        // Headers must separate from the body via a blank line (\r\n\r\n)
        char response[] = 
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=UTF-8\r\n"
            "Content-Length: 53\r\n"
            "Connection: close\r\n"
            "\r\n" // Crucial empty line separator
            "<!DOCTYPE html><html><body><h1>Hello, World!</h1></body></html>";

        // Write the response back to the client
        write(client_fd, response, strlen(response));

        // Close the client connection (HTTP/1.1 close behavior)
        close(client_fd);
    }

    // Clean up server socket (unreachable in infinite loop but good practice)
    close(server_fd);
    return 0;
}

