import wayfire_socket as ws
import os
import json

addr = os.getenv('WAYFIRE_SOCKET')
sock = ws.WayfireSocket(addr)

query = ws.get_msg_template('stipc/create_wayland_output')
response = sock.send_json(query)
print(response)
