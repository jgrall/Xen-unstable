import sys

PASS_BY_VALUE = 1
PASS_BY_REFERENCE = 2

DIR_NONE = 0
DIR_IN   = 1
DIR_OUT  = 2
DIR_BOTH = 3

class Type(object):
    def __init__(self, typename, **kwargs):
        self.comment = kwargs.setdefault('comment', None)
        self.namespace = kwargs.setdefault('namespace', "libxl_")
        self.dir = kwargs.setdefault('dir', DIR_BOTH)
        if self.dir not in [DIR_NONE, DIR_IN, DIR_OUT, DIR_BOTH]:
            raise ValueError

        self.passby = kwargs.setdefault('passby', PASS_BY_VALUE)
        if self.passby not in [PASS_BY_VALUE, PASS_BY_REFERENCE]:
            raise ValueError

        if typename is None: # Anonymous type
            self.typename = None
            self.rawname = None
        elif self.namespace is None: # e.g. system provided types
            self.typename = typename
            self.rawname = typename
        else:
            self.typename = self.namespace + typename
            self.rawname = typename

        if self.typename is not None:
            self.destructor_fn = kwargs.setdefault('destructor_fn', self.typename + "_destroy")
        else:
            self.destructor_fn = kwargs.setdefault('destructor_fn', None)

        self.autogenerate_destructor = kwargs.setdefault('autogenerate_destructor', True)

    def marshal_in(self):
        return self.dir in [DIR_IN, DIR_BOTH]
    def marshal_out(self):
        return self.dir in [DIR_OUT, DIR_BOTH]

    def make_arg(self, n, passby=None):
        if passby is None: passby = self.passby
        
        if passby == PASS_BY_REFERENCE:
            return "%s *%s" % (self.typename, n)
        else:
            return "%s %s" % (self.typename, n)
        
class Builtin(Type):
    """Builtin type"""
    def __init__(self, typename, **kwargs):
        kwargs.setdefault('destructor_fn', None)
        kwargs.setdefault('autogenerate_destructor', False)
        Type.__init__(self, typename, **kwargs)

class Number(Builtin):
    def __init__(self, ctype, **kwargs):
        kwargs.setdefault('namespace', None)
        kwargs.setdefault('destructor_fn', None)
        kwargs.setdefault('signed', False)
        self.signed = kwargs['signed']
        Builtin.__init__(self, ctype, **kwargs)

class UInt(Number):
    def __init__(self, w, **kwargs):
        kwargs.setdefault('namespace', None)
        kwargs.setdefault('destructor_fn', None)
        Number.__init__(self, "uint%d_t" % w, **kwargs)

        self.width = w

class EnumerationValue(object):
    def __init__(self, enum, value, name, **kwargs):
        self.enum = enum

        self.valuename = str.upper(name)
        self.rawname = str.upper(enum.rawname) + "_" + self.valuename
        self.name = str.upper(enum.namespace) + self.rawname
        self.value = value
        self.comment = kwargs.setdefault("comment", None)
        
class Enumeration(Type):
    def __init__(self, typename, values, **kwargs):
        kwargs.setdefault('destructor_fn', None)
        Type.__init__(self, typename, **kwargs)

        self.values = []
        for v in values:
            # (value, name[, comment=None])
            if len(v) == 2:
                (num,name) = v
                comment = None
            elif len(v) == 3:
                num,name,comment = v
            else:
                raise ""
            self.values.append(EnumerationValue(self, num, name,
                                                comment=comment,
                                                typename=self.rawname))
        
class Field(object):
    """An element of an Aggregate type"""
    def __init__(self, type, name, **kwargs):
        self.type = type
        self.name = name
        self.const = kwargs.setdefault('const', False)
        self.comment = kwargs.setdefault('comment', None)
        self.keyvar_expr = kwargs.setdefault('keyvar_expr', None)

class Aggregate(Type):
    """A type containing a collection of other types"""
    def __init__(self, kind, typename, fields, **kwargs):
        Type.__init__(self, typename, **kwargs)

        self.kind = kind

        self.fields = []
        for f in fields:
            # (name, type[, const=False[, comment=None]])
            if len(f) == 2:
                n,t = f
                const = False
                comment = None
            elif len(f) == 3:
                n,t,const = f
                comment = None
            else:
                n,t,const,comment = f
            self.fields.append(Field(t,n,const=const,comment=comment))

class Struct(Aggregate):
    def __init__(self, name, fields, **kwargs):
        kwargs.setdefault('passby', PASS_BY_REFERENCE)
        Aggregate.__init__(self, "struct", name, fields, **kwargs)

class Union(Aggregate):
    def __init__(self, name, fields, **kwargs):
        # Generally speaking some intelligence is required to free a
        # union therefore any specific instance of this class will
        # need to provide an explicit destructor function.
        kwargs.setdefault('passby', PASS_BY_REFERENCE)
        kwargs.setdefault('destructor_fn', None)
        Aggregate.__init__(self, "union", name, fields, **kwargs)

class KeyedUnion(Aggregate):
    """A union which is keyed of another variable in the parent structure"""
    def __init__(self, name, keyvar_name, fields, **kwargs):
        Aggregate.__init__(self, "union", name, [], **kwargs)

        self.keyvar_name = keyvar_name

        for f in fields:
            # (name, keyvar_expr, type)

            # keyvar_expr must contain exactly one %s which will be replaced with the keyvar_name

            n, kve, ty = f
            self.fields.append(Field(ty, n, keyvar_expr=kve))

#
# Standard Types
#

void = Builtin("void *", namespace = None)
bool = Builtin("bool", namespace = None)
size_t = Number("size_t", namespace = None)

integer = Number("int", namespace = None, signed = True)

uint8 = UInt(8)
uint16 = UInt(16)
uint32 = UInt(32)
uint64 = UInt(64)

string = Builtin("char *", namespace = None, destructor_fn = "free")

class OrderedDict(dict):
    """A dictionary which remembers insertion order.

       push to back on duplicate insertion"""

    def __init__(self):
        dict.__init__(self)
        self.__ordered = []

    def __setitem__(self, key, value):
        try:
            self.__ordered.remove(key)
        except ValueError:
            pass

        self.__ordered.append(key)
        dict.__setitem__(self, key, value)

    def ordered_keys(self):
        return self.__ordered
    def ordered_values(self):
        return [self[x] for x in self.__ordered]
    def ordered_items(self):
        return [(x,self[x]) for x in self.__ordered]

def parse(f):
    print >>sys.stderr, "Parsing %s" % f

    globs = {}
    locs = OrderedDict()

    for n,t in globals().items():
        if isinstance(t, Type):
            globs[n] = t
        elif isinstance(t,type(object)) and issubclass(t, Type):
            globs[n] = t
        elif n in ['PASS_BY_REFERENCE', 'PASS_BY_VALUE',
                   'DIR_NONE', 'DIR_IN', 'DIR_OUT', 'DIR_BOTH']:
            globs[n] = t

    try:
        execfile(f, globs, locs)
    except SyntaxError,e:
        raise SyntaxError, \
              "Errors were found at line %d while processing %s:\n\t%s"\
              %(e.lineno,f,e.text)

    types = [t for t in locs.ordered_values() if isinstance(t,Type)]

    builtins = [t for t in types if isinstance(t,Builtin)]
    types = [t for t in types if not isinstance(t,Builtin)]

    return (builtins,types)
