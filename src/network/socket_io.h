#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

// Forward-declare IXWebSocket to keep this header dependency-free.
namespace ix { class WebSocket; }

/// Minimal Socket.IO v3/v4 client transported over a plain WebSocket.
///
/// Protocol layers (outer → inner):
///   WebSocket frame → Engine.IO v4 packet → Socket.IO packet
///
/// Engine.IO v4 packet types (single ASCII digit prefix):
///   '0' OPEN    – server → client; JSON session info
///   '1' CLOSE
///   '2' PING    – server → client; reply with PONG ('3')
///   '3' PONG
///   '4' MESSAGE – carries a Socket.IO packet
///
/// Socket.IO packet types (immediately after the '4' MESSAGE prefix):
///   '0' CONNECT        – client sends with optional auth; server echoes as ACK
///   '1' DISCONNECT
///   '2' EVENT          – JSON array: ["event_name", data]
///   '4' CONNECT_ERROR
///
/// All public methods are safe to call from any thread.
/// Event callbacks are invoked from the IXWebSocket background thread.
class SocketIO {
public:
    /// Invoked with the JSON-serialised data value for the event.
    /// Empty string if the event carries no data.
    using EventCallback = std::function<void(const std::string& dataJson)>;

    SocketIO();
    ~SocketIO();

    // Non-copyable, non-movable (owns a running background thread).
    SocketIO(const SocketIO&)            = delete;
    SocketIO& operator=(const SocketIO&) = delete;

    /// Register a handler for a named Socket.IO event.
    /// May be called before connect().
    void on(const std::string& event, EventCallback cb);

    /// Called once the server acknowledges our Socket.IO CONNECT packet.
    void onConnect(std::function<void()> cb);

    /// Called when the underlying WebSocket closes (for any reason).
    void onDisconnect(std::function<void()> cb);

    /// Open a WebSocket connection to the Socket.IO server.
    ///
    /// @param host     Hostname or IP address.
    /// @param port     TCP port (typically 80 for plain ws://).
    /// @param authJson JSON object sent as the Socket.IO auth payload,
    ///                 e.g. {"role":"view","protocol_version":2}.
    ///                 Pass "{}" or omit for no auth.
    void connect(const std::string& host, int port,
                 const std::string& authJson = "{}");

    /// Close the connection and stop the background thread.
    void disconnect();

    bool isConnected() const { return sioConnected_.load(); }

    /// Emit a Socket.IO event.
    ///
    /// @param event    Event name string.
    /// @param dataJson JSON value to attach (object, array, scalar, ...).
    ///                 If empty, only the event name array element is sent.
    void emit(const std::string& event, const std::string& dataJson = "");

private:
    void onRawMessage(const std::string& msg);
    void dispatchEvent(const std::string& name, const std::string& dataJson);
    void sendRaw(const std::string& packet);

    std::unique_ptr<ix::WebSocket> ws_;

    std::map<std::string, EventCallback> handlers_;
    std::function<void()>                connectCb_;
    std::function<void()>                disconnectCb_;
    std::string                          authJson_;

    std::atomic<bool> sioConnected_{false};
    std::mutex        handlersMutex_;
};
