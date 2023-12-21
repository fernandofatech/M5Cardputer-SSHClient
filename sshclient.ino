#include <WiFi.h>
#include <M5Cardputer.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

const char* ssid = "Your_SSID"; // Replace with your WiFi SSID
const char* password = "Your_Password"; // Replace with your WiFi password

// SSH server configuration
const char* ssh_host = "192.168.0.188"; // Replace with your SSH server address
const char* ssh_user = "user"; // Replace with your SSH username
const char* ssh_password = "password"; // Replace with your SSH password

// M5Cardputer setup
M5Canvas canvas(&M5Cardputer.Display);
String commandBuffer = "> ";
int cursorY = 0;
const int lineHeight = 32;
unsigned long lastKeyPressMillis = 0;
const unsigned long debounceDelay = 200; // Adjust debounce delay as needed

// Function declarations
ssh_session connect_ssh(const char *host, const char *user, int verbosity);
int authenticate_console(ssh_session session, const char *password);
void sshTask(void *pvParameters);

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(1); // Set text size

    // Connect to WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }

    // Connect to SSH server
    TaskHandle_t sshTaskHandle = NULL;
    xTaskCreatePinnedToCore(sshTask, "SSH Task", 20000, NULL, 1, &sshTaskHandle, 1);
    if (sshTaskHandle == NULL) {
        Serial.println("Failed to create SSH Task");
    }

    // Initialize the cursor Y position
    cursorY = M5Cardputer.Display.getCursorY();
}

void loop() {
    M5Cardputer.update();
}

void sshTask(void *pvParameters) {
    ssh_session my_ssh_session = connect_ssh(ssh_host, ssh_user, SSH_LOG_PROTOCOL);
    if (my_ssh_session == NULL) {
        M5Cardputer.Display.println("SSH Connection failed.");
        vTaskDelete(NULL);
        return;
    }

    M5Cardputer.Display.println("SSH Connection established.");
    if (authenticate_console(my_ssh_session, ssh_password) != SSH_OK) {
        M5Cardputer.Display.println("SSH Authentication failed.");
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    M5Cardputer.Display.println("SSH Authentication succeeded.");

    // Open a new channel for the SSH session
    ssh_channel channel = ssh_channel_new(my_ssh_session);
    if (channel == NULL || ssh_channel_open_session(channel) != SSH_OK) {
        M5Cardputer.Display.println("SSH Channel open error.");
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    // Request a pseudo-terminal
    if (ssh_channel_request_pty(channel) != SSH_OK) {
        M5Cardputer.Display.println("Request PTY failed.");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    // Start a shell session
    if (ssh_channel_request_shell(channel) != SSH_OK) {
        M5Cardputer.Display.println("Request shell failed.");
        ssh_channel_close(channel);
        ssh_channel_free(channel);
        ssh_disconnect(my_ssh_session);
        ssh_free(my_ssh_session);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        M5Cardputer.update();

        // Handle keyboard input with debounce
        if (M5Cardputer.Keyboard.isChange()) {
            if (M5Cardputer.Keyboard.isPressed()) {
                unsigned long currentMillis = millis();
                if (currentMillis - lastKeyPressMillis >= debounceDelay) {
                    Keyboard_Class::KeysState status = M5Cardputer.Keyboard.keysState();

                    for (auto i : status.word) {
                        commandBuffer += i;
                        M5Cardputer.Display.print(i); // Display the character as it's typed
                        cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
                    }

                    if (status.del && commandBuffer.length() > 2) {
                        commandBuffer.remove(commandBuffer.length() - 1);
                        M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX() - 6, M5Cardputer.Display.getCursorY());
                        M5Cardputer.Display.print(" "); // Print a space to erase the last character
                        M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX() - 6, M5Cardputer.Display.getCursorY());
                        cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
                    }

                    if (status.enter) {
                        String message = commandBuffer.substring(2) + "\r\n"; // Use "\r\n" for newline
                        ssh_channel_write(channel, message.c_str(), message.length()); // Send message to SSH server

                        commandBuffer = "> ";
                        M5Cardputer.Display.print('\n'); // Move to the next line
                        cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
                    }

                    lastKeyPressMillis = currentMillis;
                }
            }
        }

        // Check if the cursor has reached the bottom of the display
        if (cursorY > M5Cardputer.Display.height() - lineHeight) {
            // Scroll the display up by one line
            M5Cardputer.Display.scroll(0, -lineHeight);

            // Reset the cursor to the new line position
            cursorY -= lineHeight;
            M5Cardputer.Display.setCursor(M5Cardputer.Display.getCursorX(), cursorY);
        }

        char buffer[1024];
        int nbytes = ssh_channel_read_nonblocking(channel, buffer, sizeof(buffer), 0);
        if (nbytes > 0) {
            for (int i = 0; i < nbytes; ++i) {
                if (buffer[i] == '\r') {
                    continue; // Handle carriage return
                }
                M5Cardputer.Display.write(buffer[i]);
                cursorY = M5Cardputer.Display.getCursorY(); // Update cursor Y position
            }
        }

        if (nbytes < 0 || ssh_channel_is_closed(channel)) {
            break;
        }
    }

    // Clean up
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    ssh_disconnect(my_ssh_session);
    ssh_free(my_ssh_session);
    vTaskDelete(NULL);
}

ssh_session connect_ssh(const char *host, const char *user, int verbosity) {
    ssh_session session = ssh_new();
    if (session == NULL) {
        Serial.println("Failed to create SSH session");
        return NULL;
    }

    ssh_options_set(session, SSH_OPTIONS_HOST, host);
    ssh_options_set(session, SSH_OPTIONS_USER, user);
    ssh_options_set(session, SSH_OPTIONS_LOG_VERBOSITY, &verbosity);

    if (ssh_connect(session) != SSH_OK) {
        Serial.print("Error connecting to host: ");
        Serial.println(ssh_get_error(session));
        ssh_free(session);
        return NULL;
    }

    return session;
}

int authenticate_console(ssh_session session, const char *password) {
    int rc = ssh_userauth_password(session, NULL, password);
    if (rc != SSH_AUTH_SUCCESS) {
        Serial.print("Error authenticating with password: ");
        Serial.println(ssh_get_error(session));
        return rc;
    }
    return SSH_OK;
}