# Copyright (C) 2004 Mike Wray <mike.wray@hp.com>

import httplib
import types

from encode import *
import sxp

DEBUG = 0

HTTP_OK                              = 200
HTTP_CREATED                         = 201
HTTP_ACCEPTED                        = 202
HTTP_NO_CONTENT                      = 204

class XendError(RuntimeError):
    """Error class for 'expected errors' when talking to xend.
    """
    pass

class XendRequest:
    """A request to xend.
    """

    def __init__(self, url, method, args):
        """Create a request. Sets up the headers, argument data, and the
        url.

        @param url:    the url to request
        @param method: request method, GET or POST
        @param args:   dict containing request args, if any
        """
        if url.proto != 'http':
            raise ValueError('Invalid protocol: ' + url.proto)
        (hdr, data) = encode_data(args)
        if args and method == 'GET':
            url.query = data
            data = None
        if method == "POST" and url.path.endswith('/'):
            url.path = url.path[:-1]

        self.headers = hdr
        self.data = data
        self.url = url
        self.method = method

class XendClientProtocol:
    """Abstract class for xend clients.
    """
    def xendRequest(self, url, method, args=None):
        """Make a request to xend.
        Implement in a subclass.

        @param url:    xend request url
        @param method: http method: POST or GET
        @param args:   request arguments (dict)
        """
        raise NotImplementedError()

    def xendGet(self, url, args=None):
        """Make a xend request using HTTP GET.
        Requests using GET are usually 'safe' and may be repeated without
        nasty side-effects.

        @param url:    xend request url
        @param data:   request arguments (dict)
        """
        return self.xendRequest(url, "GET", args)

    def xendPost(self, url, args):
        """Make a xend request using HTTP POST.
        Requests using POST potentially cause side-effects, and should
        not be repeated unless you really want to repeat the side
        effect.

        @param url:    xend request url
        @param args:   request arguments (dict)
        """
        return self.xendRequest(url, "POST", args)

    def handleStatus(self, version, status, message):
        """Handle the status returned from the request.
        """
        status = int(status)
        if status in [ HTTP_NO_CONTENT ]:
            return None
        if status not in [ HTTP_OK, HTTP_CREATED, HTTP_ACCEPTED ]:
            return self.handleException(XendError(message))
        return 'ok'

    def handleResponse(self, data):
        """Handle the data returned in response to the request.
        """
        if data is None: return None
        type = self.getHeader('Content-Type')
        if type != sxp.mime_type:
            return data
        try:
            pin = sxp.Parser()
            pin.input(data);
            pin.input_eof()
            val = pin.get_val()
        except sxp.ParseError, err:
            return self.handleException(err)
        if isinstance(val, types.ListType) and sxp.name(val) == 'xend.err':
            err = XendError(val[1])
            return self.handleException(err)
        return val

    def handleException(self, err):
        """Handle an exception during the request.
        May be overridden in a subclass.
        """
        raise err

    def getHeader(self, key):
        """Get a header from the response.
        Case is ignored in the key.

        @param key: header key
        @return: header
        """
        raise NotImplementedError()

class SynchXendClientProtocol(XendClientProtocol):
    """A synchronous xend client. This will make a request, wait for
    the reply and return the result.
    """

    resp = None

    def xendRequest(self, url, method, args=None):
        """Make a request to xend.

        @param url:    xend request url
        @param method: http method: POST or GET
        @param args:   request arguments (dict)
        """
        self.request = XendRequest(url, method, args)
        conn = httplib.HTTPConnection(url.location())
        if DEBUG: conn.set_debuglevel(1)
        conn.request(method, url.fullpath(), self.request.data, self.request.headers)
        resp = conn.getresponse()
        self.resp = resp
        val = self.handleStatus(resp.version, resp.status, resp.reason)
        if val is None:
            data = None
        else:
            data = resp.read()
        conn.close()
        val = self.handleResponse(data)
        return val

    def getHeader(self, key):
        return self.resp.getheader(key)

