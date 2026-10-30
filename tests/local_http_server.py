"""Loopback-only HTTP fixtures must not depend on external name resolution."""

import http.server
import socketserver


class LocalHTTPServer(http.server.ThreadingHTTPServer):
    def server_bind(self):
        socketserver.TCPServer.server_bind(self)
        self.server_name = "localhost"
        self.server_port = self.server_address[1]
