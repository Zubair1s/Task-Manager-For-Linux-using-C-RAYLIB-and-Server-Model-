# Linux Task Manager (C++ | raylib)

A simple Linux Task Manager built in C++ using raylib, focused on real-time system monitoring with an added remote alert feature.

## Features

* Real-time CPU, Memory, and Disk usage monitoring
* View and manage running processes
* Kill processes directly from the UI
* CPU temperature tracking
* Remote alert system (sends message to server when:

  * CPU temperature exceeds 80°C
  * A process consumes high CPU)

---

## How to Run

### 1. Start the Server First

Make sure your server is running before launching the task manager.

* You can use:

  * `localhost` (127.0.0.1) for local testing
  * Or your system’s IP address for remote monitoring

---

### 2. Compile the Project

Make sure you have **raylib installed**, then compile:

```bash
g++ main.cpp -o taskmanager -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
```

---

### 3. Run the Application

```bash
./taskmanager
```

When prompted:

```
Enter server IP:
```

* Enter:

  * `127.0.0.1` (for local server)
  * Or your server’s IP address

---

## Notes

* The server must be running on port **8080**
* The application will automatically send alerts when thresholds are exceeded
* Designed for Linux systems

---


## Future Improvements

* Better UI enhancements
* Logging system
* Cross-platform support

---

## Author

Zubair Ahmed
