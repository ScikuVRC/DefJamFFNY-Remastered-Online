"""Compile the router port opener (src/hooks/netplay_router.c) and run it against a fake UPnP
router on this machine: the description it fetches, the commands it sends, and what it makes of
the replies, including a router that takes only permanent forwards and one behind another NAT."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import subprocess
import threading

import pytest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r'''
#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#define nap() Sleep(50)
#else
#include <unistd.h>
#define nap() usleep(50000)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netplay_router.h"
static char input[65536];
int main(int argc, char **argv)
{
    char out[512], service[128];
    size_t n = 0;
    int state, i;
#if defined(_WIN32)
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#endif
    if (!strcmp(argv[1], "open")) {
        netplay_router_open((unsigned short)atoi(argv[2]));
        for (i = 0; i < 300 && (state = netplay_router_status(out, sizeof out)) == NETPLAY_ROUTER_TRYING; i++)
            nap();
        printf("%d|%s\n", state, out);
        netplay_router_close();
        printf("%d\n", netplay_router_status(out, sizeof out));
        return 0;
    }
    n = fread(input, 1, sizeof input - 1, stdin);
    input[n] = 0;
    if (!strcmp(argv[1], "location"))
        printf("%d|%s\n", netplay_router_location(input, out, sizeof out), out);
    else if (!strcmp(argv[1], "control")) {
        int ok = netplay_router_control(input, argv[2], out, sizeof out, service, sizeof service);
        printf("%d|%s|%s\n", ok, ok ? out : "", ok ? service : "");
    } else if (!strcmp(argv[1], "value")) {
        int ok = netplay_router_value(input, argv[2], out, sizeof out);
        printf("%d|%s\n", ok, ok ? out : "");
    } else if (!strcmp(argv[1], "dechunk")) {
        n = netplay_router_dechunk(input, n);
        printf("%u|%s\n", (unsigned)n, input);
    }
    return 0;
}
'''

SERVICE = 'urn:schemas-upnp-org:service:WANIPConnection:1'

DESCRIPTION = '''<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
<controlURL>/ctl/L3F</controlURL></service></serviceList>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList>
<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType><controlURL>/ctl/PPPConn</controlURL></service>
<service>
  <serviceType> urn:schemas-upnp-org:service:WANIPConnection:1 </serviceType>
  <controlURL>/ctl/IPConn</controlURL>
</service>
</serviceList></device></deviceList></device></deviceList></device></root>
'''


def envelope(action, inner):
    return ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body>'
            '<u:%sResponse xmlns:u="%s">%s</u:%sResponse></s:Body></s:Envelope>' % (action, SERVICE, inner, action))


FAULT = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/"><s:Body><s:Fault>'
         '<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>'
         '<UPnPError xmlns="urn:schemas-upnp-org:control-1-0"><errorCode>%d</errorCode>'
         '<errorDescription>%s</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>')


@pytest.fixture(scope='module')
def tool(tmp_path_factory):
    compiler = shutil.which('cl') if os.name == 'nt' else shutil.which('cc')
    if not compiler:
        pytest.skip('the router fixture needs a C compiler')
    folder = tmp_path_factory.mktemp('netplay_router')
    driver = folder / 'driver.c'
    driver.write_text(DRIVER)
    exe = folder / ('router.exe' if os.name == 'nt' else 'router')
    sources = [driver, ROOT / 'src/hooks/netplay_router.c']
    if os.name == 'nt':
        cmd = [compiler, '/nologo', '/TC', '/I' + str(ROOT / 'src/hooks'), *map(str, sources), '/Fe:' + str(exe),
               '/link', 'ws2_32.lib']
    else:
        cmd = [compiler, '-I' + str(ROOT / 'src/hooks'), *map(str, sources), '-o', str(exe), '-pthread']
    result = subprocess.run(cmd, cwd=folder, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr

    def run(*args, stdin='', env=None):
        e = {k: v for k, v in os.environ.items() if not k.startswith('RECOMP_')}
        e.update(env or {})
        return subprocess.run([str(exe), *map(str, args)], input=stdin, capture_output=True, text=True, env=e,
                              timeout=60, check=True).stdout.splitlines()
    return run


def test_location_of_a_search_reply(tool):
    reply = ('HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nlocation:  http://192.168.1.1:5000/rootDesc.xml\r\n'
             'ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n')
    assert tool('location', stdin=reply) == ['1|http://192.168.1.1:5000/rootDesc.xml']
    assert tool('location', stdin='HTTP/1.1 200 OK\r\nST: x\r\n\r\n')[0].startswith('0|')


def test_control_prefers_the_ip_connection_and_makes_its_url_absolute(tool):
    out = tool('control', 'http://192.168.1.1:5000/rootDesc.xml', stdin=DESCRIPTION)
    assert out == ['1|http://192.168.1.1:5000/ctl/IPConn|' + SERVICE]
    ppp_only = DESCRIPTION.replace('WANIPConnection', 'Other')
    assert tool('control', 'http://192.168.1.1:5000/d.xml', stdin=ppp_only) == [
        '1|http://192.168.1.1:5000/ctl/PPPConn|urn:schemas-upnp-org:service:WANPPPConnection:1']
    based = DESCRIPTION.replace('<device>', '<URLBase>http://10.0.0.1:49000/</URLBase><device>', 1)
    assert tool('control', 'http://192.168.1.1:5000/d.xml', stdin=based)[0].split('|')[1] == 'http://10.0.0.1:49000/ctl/IPConn'
    assert tool('control', 'http://h/d.xml', stdin='<root></root>') == ['0||']


def test_values_whatever_their_prefix(tool):
    reply = envelope('GetExternalIPAddress', '<NewExternalIPAddress> 203.0.113.5 </NewExternalIPAddress>')
    assert tool('value', 'NewExternalIPAddress', stdin=reply) == ['1|203.0.113.5']
    assert tool('value', 'errorCode', stdin=FAULT % (718, 'ConflictInMappingEntry')) == ['1|718']
    assert tool('value', 'NewRemoteHost', stdin='<a><NewRemoteHost/></a>') == ['1|']
    assert tool('value', 'Missing', stdin=reply) == ['0|']


def test_chunks_are_joined(tool):
    assert tool('dechunk', stdin='5\r\nhello\r\n6;ext=1\r\n world\r\n0\r\n\r\n') == ['11|hello world']


class Router(BaseHTTPRequestHandler):
    """A home router's UPnP side: its description, and the WAN IP connection's commands."""
    def log_message(self, *args):
        pass

    def reply(self, status, body, chunked=False):
        data = body.encode()
        self.send_response(status)
        self.send_header('Content-Type', 'text/xml')
        if chunked:
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for i in range(0, len(data), 100):
                piece = data[i:i + 100]
                self.wfile.write(b'%x\r\n%s\r\n' % (len(piece), piece))
            self.wfile.write(b'0\r\n\r\n')
        else:
            self.send_header('Content-Length', str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def do_GET(self):
        self.reply(200, DESCRIPTION, chunked=self.server.chunked)

    def do_POST(self):
        body = self.rfile.read(int(self.headers['Content-Length'])).decode()
        action = self.headers['SOAPAction'].strip('"').split('#')[1]
        self.server.calls.append((self.path, action, body))
        if action == 'AddPortMapping':
            if self.server.permanent_only and '<NewLeaseDuration>0<' not in body:
                return self.reply(500, FAULT % (725, 'OnlyPermanentLeasesSupported'))
            return self.reply(200, envelope(action, ''))
        if action == 'GetExternalIPAddress':
            return self.reply(200, envelope(action, '<NewExternalIPAddress>%s</NewExternalIPAddress>' % self.server.external))
        return self.reply(200, envelope(action, ''))


@pytest.fixture
def router():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Router)
    server.calls, server.permanent_only, server.chunked, server.external = [], False, False, '203.0.113.5'
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield server
    server.shutdown()


def open_port(tool, router, port=7777):
    location = 'http://127.0.0.1:%d/rootDesc.xml' % router.server_address[1]
    return tool('open', port, env={'RECOMP_NETPLAY_UPNP': '1', 'RECOMP_NETPLAY_UPNP_LOCATION': location})


def test_opens_the_port_reads_the_address_and_closes_it_again(tool, router):
    router.chunked = True
    assert open_port(tool, router) == ['2|203.0.113.5:7777', '0']
    actions = [c[1] for c in router.calls]
    assert actions == ['AddPortMapping', 'GetExternalIPAddress', 'DeletePortMapping']
    add = router.calls[0][2]
    assert router.calls[0][0] == '/ctl/IPConn'
    for part in ('<NewExternalPort>7777<', '<NewInternalPort>7777<', '<NewProtocol>TCP<',
                 '<NewInternalClient>127.0.0.1<', '<NewLeaseDuration>14400<'):
        assert part in add
    assert '<NewExternalPort>7777<' in router.calls[2][2]


def test_a_router_that_takes_only_permanent_forwards(tool, router):
    router.permanent_only = True
    assert open_port(tool, router)[0] == '2|203.0.113.5:7777'
    leases = [c[2].split('<NewLeaseDuration>')[1].split('<')[0] for c in router.calls if c[1] == 'AddPortMapping']
    assert leases == ['14400', '0']


def test_a_router_behind_another_nat_says_so_and_takes_its_forward_back(tool, router):
    router.external = '100.64.3.9'
    state, text = open_port(tool, router)[0].split('|', 1)
    assert state == '3' and 'behind another' in text and '100.64.3.9' in text
    assert [c[1] for c in router.calls] == ['AddPortMapping', 'GetExternalIPAddress', 'DeletePortMapping']


def test_left_alone_when_turned_off(tool, router):
    location = 'http://127.0.0.1:%d/rootDesc.xml' % router.server_address[1]
    assert tool('open', 7777, env={'RECOMP_NETPLAY_UPNP': '0', 'RECOMP_NETPLAY_UPNP_LOCATION': location}) == ['0|', '0']
    assert tool('open', 7777, env={'RECOMP_SETTINGS': 'none', 'RECOMP_NETPLAY_UPNP_LOCATION': location}) == ['0|', '0']
    assert router.calls == []
