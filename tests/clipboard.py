"""
Usage:
 run with --help

Example invocations:

Test client paste:
 guest side, listen for connection, compare paste to socketed data.
 python clipboard.py --server
 host side, connect to other side, paste messages of 20000 bytes (defaults to 1000, settable with -N).
 python clipboard.py --client --dest <guest ip> --paste -n 20000

To test guest paste, put --paste on guest invocation and remove from client.
"""

import sys
import hashlib
import socket
import random
import os
import cPickle
import subprocess
import time

first_port = 9876
ports_tried = 10

N = 1000

verbose = False
opts = None    # options set from command line

global xsel_process
xsel_process = None

def win_set_pastebuffer(msg, aType=None):
    import win32clipboard as w
    import win32con
    if aType is None:
        aType = win32con.CF_TEXT
    w.OpenClipboard()
    w.EmptyClipboard()
    w.SetClipboardData(aType, msg) 
    w.CloseClipboard()

def win_get_pastebuffer():
    import win32clipboard as w
    import win32con
    w.OpenClipboard()
    d = w.GetClipboardData(win32con.CF_TEXT)
    w.CloseClipboard()
    return d

def x11_get_pastebuffer():
    s=subprocess.Popen('xsel', stdout=subprocess.PIPE)
    sel = s.stdout.read()
    s.wait()
    return sel

def x11_set_pastebuffer(msg):
    global spicec_window
    global console_window
    global xsel_process
    if xsel_process:
        if verbose:
            print "killing %s" % xsel_process.pid
        xsel_process.terminate()
        xsel_process.wait()
    xsel_process = subprocess.Popen('xsel', stdin=subprocess.PIPE)
    xsel_process.stdin.write(msg)
    xsel_process.stdin.close()
    time.sleep(opts.target_pre_activate)
    # now activate spice window so the text is actually sent to the guest
    spicec_window.activate(0) # needs a timestamp, can't figure out the real one, zero works with a warning
    time.sleep(opts.target_post_activate)
    console_window.activate(0)
    time.sleep(opts.console_post_activate)
    # we kill xsel on the next iteration (thereby killing the pastebuffer)

def get_both_windows():
    import wnck
    import gtk
    import gobject
    def getonce():
        s = wnck.screen_get_default()
        ws = s.get_windows()
        return ws
    w = [w for w in getonce() if w.get_name().startswith('SPICEc:0')]
    # run gtk event loop long enough to register all window names
    gobject.idle_add(gtk.main_quit)
    gtk.main()
    w = [w for w in getonce() if w.get_name().startswith('SPICEc:0')]
    if len(w) != 1:
        if len(w) > 1:
            print "more then one spice window:", w
            import pdb; pdb.set_trace()
        else:
            print "no spice windows"
            import pdb; pdb.set_trace()
            sys.exit(-1)
    return w[0], wnck.screen_get_default().get_active_window()

if sys.platform == 'win32':
    set_pastebuffer = win_set_pastebuffer
    get_pastebuffer = win_get_pastebuffer
else:
    global spicec_window
    global console_window
    spicec_window, console_window = get_both_windows()
    set_pastebuffer = x11_set_pastebuffer
    get_pastebuffer = x11_get_pastebuffer

def compute_md5(msg):
    return hashlib.md5(msg).hexdigest()

def set_and_md5(msg):
    h = compute_md5(msg)
    set_pastebuffer(msg)
    return h

def paste_and_md5_random_string(n):
    s = random.randint(0,1000)
    msg = ''.join([str(x+s) for x in xrange(n)])[:n]
    return set_and_md5(msg), msg

def getsock(HOST, PORT):
    s = None
    for res in socket.getaddrinfo(HOST, PORT, socket.AF_UNSPEC, socket.SOCK_STREAM):
        af, socktype, proto, canonname, sa = res
        try:
            s = socket.socket(af, socktype, proto)
        except socket.error, msg:
            s = None
            continue
        try:
            s.connect(sa)
        except socket.error, msg:
            s.close()
            s = None
            continue
        break
    return s

def client(opts):
    print "starting client - %s:%s-%s" % (opts.dest, first_port, first_port+ports_tried-1)
    for p in xrange(first_port, first_port + ports_tried):
        s=getsock(opts.dest, p)
        if s:
            break
    print "port = %s" % p
    if not s:
        print "could not open socket"
        exit(-1)
    return s

def paste_many(s, n, N):
    print ">>> start pasting <<<"
    for i in xrange(N):
        if i % 10 == 0:
            print "%s/%s" % (i, N)
        md5, msg = paste_and_md5_random_string(n)
        s.send(cPickle.dumps((md5,msg)) + '\n')
        answer = cPickle.loads(s.recv(1024))
        if answer != md5:
            print "%s: failed: sent %s, got %s" % (i, md5, answer)
            break
    print "%s passed" % N

def sign_pasted(s, paste=False):
    print ">>> start signing <<<"
    i = 0
    last_paste = ''
    while True:
        i += 1
        pasted_md5, pasted_from_file = pickle_from_socket(s)
        pastebuffer = get_pastebuffer()
        waits = 0
        # sometimes it takes a while for the new paste buffer to register
        # - needs further investigation
        while ((last_paste == pastebuffer
               or not set(pastebuffer) <= numbers)
               and waits < 30):
            print "!",pastebuffer[:20]
            waits += 1
            time.sleep(0.2)
            pastebuffer = get_pastebuffer()
        last_paste = pastebuffer
   
        if pastebuffer != pasted_from_file:
            import pdb; pdb.set_trace()
        computed_md5 = compute_md5(pastebuffer)
        s.send(cPickle.dumps(computed_md5) + '\n')
        if computed_md5 != pasted_md5:
            print "%s: error on text of length %s" % (i, len(pastebuffer))
            import pdb; pdb.set_trace()
        if verbose:
            print i

def getopts():
    global verbose
    import optparse
    import sys
    parser = optparse.OptionParser()
    parser.add_option('--paste', action='store_true', help="do paste to other side (if not present sign other side and send back)")
    parser.add_option('--server', action='store_true', help="listen to socket")
    parser.add_option('--client', action='store_true', help="connect to socket")
    parser.add_option('--paste1', action='store_true', help="do a single send")
    parser.add_option('--sleep1', type='float', help="sleep after paste1")
    parser.add_option('--get1', action='store_true', help="do a single read")
    parser.add_option('--dest', help="destination address for client")
    parser.add_option('-N', default=N, type='int', help="set number of iterations")
    parser.add_option('-n', default=100, type='int', help="set length of test strings")
    parser.add_option('-v', action='count', default=False, help="be more verbose")
    parser.add_option('--target_pre_activate', type='float', default=0.1, help='time to sleep before target activation')
    parser.add_option('--target_post_activate', type='float', default=0.1, help='time to sleep after target activation')
    parser.add_option('--console_post_activate', type='float', default=0.1, help='time to sleep before console activation')
    opts, rest = parser.parse_args(sys.argv[1:])
    if opts.v is not None:
        verbose = opts.v
    is_server = opts.server
    if opts.client and opts.dest is None:
        parser.print_help()
        raise SystemExit
    return is_server, opts

def accept_one(port):
    HOST = None
    PORT = port
    s = None
    for res in socket.getaddrinfo(HOST, PORT, socket.AF_UNSPEC,
                                  socket.SOCK_STREAM, 0, socket.AI_PASSIVE):
        af, socktype, proto, canonname, sa = res
        try:
            s = socket.socket(af, socktype, proto)
        except socket.error, msg:
            print msg
            s = None
            continue
        try:
            s.bind(sa)
            s.listen(1)
        except socket.error, msg:
            print msg
            s.close()
            s = None
            continue
        break
    if s is None:
        print 'could not open socket'
        return None
    conn, addr = s.accept()
    print 'Connected by', addr
    return conn

numbers = set('0123456789')

def pickle_from_socket(s):
    """ for buffers larger then a certain size you get multiple
    intermediate results from recv - concat them together """
    getmore = True
    r = []
    while getmore:
      r.append(s.recv(1024*1024))
      if r[-1] == '':
        print "socket closed, exiting"
        sys.exit(0)
      try:
        res = cPickle.loads(''.join(r))
      except:
        pass
      else:
        break
    return res

def run_socket(s, opts):
    if opts.paste:
        paste_many(s, n=opts.n, N=opts.N)
    else:
        sign_pasted(s)

def run_client(opts):
    run_socket(client(opts), opts=opts)

def run_server(opts):
    import sys
    for p in xrange(first_port, first_port+ports_tried):
        s = accept_one(p)
        if s:
           break
    assert(s is not None)
    print "port = %s" % p
    run_socket(s, opts)

if __name__ == '__main__':
    is_server, opts = getopts()
    if opts.paste1:
        paste_and_md5_random_string(opts.n)
        if opts.sleep1:
            time.sleep(opts.sleep1)
    elif opts.get1:
        pastebuffer = get_pastebuffer()
        computed_md5 = compute_md5(pastebuffer)
        print "got %d bytes, md5 = %s" % (len(pastebuffer), computed_md5)
    elif is_server:
        run_server(opts)
    else:
        run_client(opts)

