#!/usr/bin/python

import sys,os

import libxltypes

# typename -> ( ocaml_type, c_from_ocaml, ocaml_from_c )
builtins = {
    "bool":                 ("bool",                   "%(c)s = Bool_val(%(o)s)",           "Val_bool(%(c)s)" ),
    "int":                  ("int",                    "%(c)s = Int_val(%(o)s)",            "Val_int(%(c)s)"  ),
    "char *":               ("string",                 "%(c)s = dup_String_val(gc, %(o)s)", "caml_copy_string(%(c)s)"),
    "libxl_domid":          ("domid",                  "%(c)s = Int_val(%(o)s)",            "Val_int(%(c)s)"  ),
    "libxl_uuid":           ("int array",              "Uuid_val(gc, lg, &%(c)s, %(o)s)",   "Val_uuid(&%(c)s)"),
    "libxl_key_value_list": ("(string * string) list", None,                                None),
    "libxl_mac":            ("int array",              "Mac_val(gc, lg, &%(c)s, %(o)s)",    "Val_mac(&%(c)s)"),
    "libxl_hwcap":          ("int32 array",            None,                                "Val_hwcap(&%(c)s)"),
    }

DEVICE_FUNCTIONS = [ ("add",            ["t", "domid", "unit"]),
                     ("remove",         ["t", "domid", "unit"]),
                     ("destroy",        ["t", "domid", "unit"]),
                   ]

functions = { # ( name , [type1,type2,....] )
    "device_vfb":     [ ("add",            ["t", "domid", "unit"]),
                        ("clean_shutdown", ["domid", "unit"]),
                        ("hard_shutdown",  ["domid", "unit"]),
                      ],
    "device_vkb":     DEVICE_FUNCTIONS,
    "device_disk":    DEVICE_FUNCTIONS,
    "device_nic":     DEVICE_FUNCTIONS,
    "device_pci":     [ ("add",            ["t", "domid", "unit"]),
                        ("remove",         ["t", "domid", "unit"]),
                        ("shutdown",       ["domid", "unit"]),
                      ],
    "physinfo":       [ ("get",            ["unit", "t"]),
                      ],
    "sched_credit":   [ ("domain_get",     ["domid", "t"]),
                        ("domain_set",     ["domid", "t", "unit"]),
                      ],
}
def stub_fn_name(ty, name):
    return "stub_xl_%s_%s" % (ty.rawname,name)
    
def ocaml_type_of(ty):
    if ty.rawname == "domid":
        return "domid"
    elif isinstance(ty,libxltypes.UInt):
        if ty.width in [8, 16]:
            # handle as ints
            width = None
        elif ty.width in [32, 64]:
            width = ty.width
        else:
            raise NotImplementedError("Cannot handle %d-bit int" % ty.width)
        if width:
            return "int%d" % ty.width
        else:
            return "int"

    elif isinstance(ty,libxltypes.Builtin):
        if not builtins.has_key(ty.typename):
            raise NotImplementedError("Unknown Builtin %s (%s)" % (ty.typename, type(ty)))
        typename,_,_ = builtins[ty.typename]
        if not typename:
            raise NotImplementedError("No typename for Builtin %s (%s)" % (ty.typename, type(ty)))
        return typename
    else:
        return ty.rawname

def ocaml_instance_of(type, name):
    return "%s : %s" % (name, ocaml_type_of(type))

def gen_ocaml_ml(ty, interface, indent=""):

    if interface:
        s = ("""(* %s interface *)\n""" % ty.typename)
    else:
        s = ("""(* %s implementation *)\n""" % ty.typename)
    if isinstance(ty, libxltypes.Enumeration):
        s = "type %s = \n" % ty.rawname
        for v in ty.values:
            s += "\t | %s\n" % v.rawname
    elif isinstance(ty, libxltypes.Aggregate):
        s = ""
        if ty.typename is None:
            raise NotImplementedError("%s has no typename" % type(ty))
        else:

            module_name = ty.rawname[0].upper() + ty.rawname[1:]

            if interface:
                s += "module %s : sig\n" % module_name
            else:
                s += "module %s = struct\n" % module_name
            s += "\ttype t =\n"
            s += "\t{\n"
            
        for f in ty.fields:
            x = ocaml_instance_of(f.type, f.name)
            x = x.replace("\n", "\n\t\t")
            s += "\t\t" + x + ";\n"

        s += "\t}\n"
        
        if functions.has_key(ty.rawname):
            for name,args in functions[ty.rawname]:
                s += "\texternal %s : " % name
                s += " -> ".join(args)
                s += " = \"%s\"\n" % stub_fn_name(ty,name)
        
        s += "end\n"

    else:
        raise NotImplementedError("%s" % type(ty))
    return s.replace("\n", "\n%s" % indent)

def c_val(ty, c, o, indent="", parent = None):
    if ty.passby == libxltypes.PASS_BY_REFERENCE:
        makeref = ""
    else:
        makeref = "&"

    s = indent
    if isinstance(ty,libxltypes.UInt):
        if ty.width in [8, 16]:
            # handle as ints
            width = None
        elif ty.width in [32, 64]:
            width = ty.width
        else:
            raise NotImplementedError("Cannot handle %d-bit int" % ty.width)
        if width:
            s += "%s = Int%d_val(%s);" % (c, width, o)
        else:
            s += "%s = Int_val(%s);" % (c, o)
    elif isinstance(ty,libxltypes.Builtin):
        if not builtins.has_key(ty.typename):
            raise NotImplementedError("Unknown Builtin %s (%s)" % (ty.typename, type(ty)))
        _,fn,_ = builtins[ty.typename]
        if not fn:
            raise NotImplementedError("No c_val fn for Builtin %s (%s)" % (ty.typename, type(ty)))
        s += "%s;" % (fn % { "o": o, "c": c })
    elif isinstance(ty,libxltypes.Enumeration) and (parent is None):
        n = 0
        s += "switch(Int_val(%s)) {\n" % o
        for e in ty.values:
            s += "    case %d: *%s = %s; break;\n" % (n, c, e.name)
            n += 1
        s += "    default: failwith_xl(\"cannot convert value to %s\", lg); break;\n" % ty.typename
        s += "}"
    elif isinstance(ty, libxltypes.Aggregate) and (parent is None):
        n = 0
        for f in ty.fields:
            s += "%s\n" % c_val(f.type, "%s->%s" % (c, f.name), "Field(%s, %d)" % (o,n), parent="%s->" % (c))
            n = n + 1
    else:
        s += "%s_val(gc, lg, %s, %s);" % (ty.rawname, makeref + c, o)
    
    return s.replace("\n", "\n%s" % indent)

def gen_c_val(ty, indent=""):
    s = "/* Convert caml value to %s */\n" % ty.rawname
    
    s += "static int %s_val (caml_gc *gc, struct caml_logger *lg, %s *c_val, value v)\n" % (ty.rawname, ty.typename)
    s += "{\n"
    s += "\tCAMLparam1(v);\n"
    s += "\n"

    s += c_val(ty, "c_val", "v", indent="\t") + "\n"
    
    s += "\tCAMLreturn(0);\n"
    s += "}\n"
    
    return s.replace("\n", "\n%s" % indent)

def ocaml_Val(ty, o, c, indent="", parent = None):
    if ty.passby == libxltypes.PASS_BY_REFERENCE:
        makeref = ""
    else:
        makeref = "&"
    
    s = indent
    if isinstance(ty,libxltypes.UInt):
        if ty.width in [8, 16]:
            # handle as ints
            width = None
        elif ty.width in [32, 64]:
            width = ty.width
        else:
            raise NotImplementedError("Cannot handle %d-bit int" % ty.width)
        if width:
            s += "%s = caml_copy_int%d(%s);" % (o, width, c)
        else:
            s += "%s = Val_int(%s);" % (o, c)
    elif isinstance(ty,libxltypes.Builtin):
        if not builtins.has_key(ty.typename):
            raise NotImplementedError("Unknown Builtin %s (%s)" % (ty.typename, type(ty)))
        _,_,fn = builtins[ty.typename]
        if not fn:
            raise NotImplementedError("No ocaml Val fn for Builtin %s (%s)" % (ty.typename, type(ty)))
        s += "%s = %s;" % (o, fn % { "c": c })
    elif isinstance(ty,libxltypes.Enumeration) and (parent is None):
        n = 0
        s += "switch(*%s) {\n" % c
        for e in ty.values:
            s += "    case %s: %s = Int_val(%d); break;\n" % (e.name, o, n)
            n += 1
        s += "    default: failwith_xl(\"cannot convert value from %s\", lg); break;\n" % ty.typename
        s += "}"
    elif isinstance(ty,libxltypes.Aggregate) and (parent is None):
        s += "{\n"
        s += "\tvalue %s_field;\n" % ty.rawname
        s += "\n"
        s += "\t%s = caml_alloc_tuple(%d);\n" % (o, len(ty.fields))
        
        n = 0
        for f in ty.fields:
            s += "\n"
            s += "\t%s\n" % ocaml_Val(f.type, "%s_field" % ty.rawname, "%s->%s" % (c,f.name), parent="%s->" % c)
            s += "\tStore_field(%s, %d, %s);\n" % (o, n, "%s_field" % ty.rawname)
            n = n + 1
        s += "}"
    else:
        s += "%s = Val_%s(gc, lg, %s);" % (o, ty.rawname, makeref + c)
    
    return s.replace("\n", "\n%s" % indent).rstrip(indent)

def gen_Val_ocaml(ty, indent=""):
    s = "/* Convert %s to a caml value */\n" % ty.rawname

    s += "static value Val_%s (caml_gc *gc, struct caml_logger *lg, %s *%s_c)\n" % (ty.rawname, ty.typename, ty.rawname)
    s += "{\n"
    s += "\tCAMLparam0();\n"
    s += "\tCAMLlocal1(%s_ocaml);\n" % ty.rawname

    s += ocaml_Val(ty, "%s_ocaml" % ty.rawname, "%s_c" % ty.rawname, indent="\t") + "\n"
    
    s += "\tCAMLreturn(%s_ocaml);\n" % ty.rawname
    s += "}\n"
    return s.replace("\n", "\n%s" % indent)

def gen_c_stub_prototype(ty, fns):
    s = "/* Stubs for %s */\n" % ty.rawname
    for name,args in fns:        
        # For N args we return one value and take N-1 values as parameters
        s += "value %s(" % stub_fn_name(ty, name)
        s += ", ".join(["value v%d" % v for v in range(1,len(args))])
        s += ");\n"
    return s

def autogen_header(open_comment, close_comment):
    s = open_comment + " AUTO-GENERATED FILE DO NOT EDIT " + close_comment + "\n"
    s += open_comment + " autogenerated by \n"
    s += reduce(lambda x,y: x + " ", range(len(open_comment + " ")), "")
    s += "%s" % " ".join(sys.argv)
    s += "\n " + close_comment + "\n\n"
    return s

if __name__ == '__main__':
    if len(sys.argv) < 4:
        print >>sys.stderr, "Usage: genwrap.py <idl> <mli> <ml> <c-inc>"
        sys.exit(1)

    idl = sys.argv[1]
    (_,types) = libxltypes.parse(idl)

    # Do not generate these yet.
    blacklist = [
        "cpupoolinfo",
        "domain_create_info",
        "domain_build_info",
        "device_model_info",
        "vcpuinfo",
        "topologyinfo",
        ]

    for t in blacklist:
        if t not in [ty.rawname for ty in types]:
            print "unknown type %s in blacklist" % t

    types = [ty for ty in types if not ty.rawname in blacklist]
    
    _ml = sys.argv[3]
    ml = open(_ml, 'w')
    ml.write(autogen_header("(*", "*)"))

    _mli = sys.argv[2]
    mli = open(_mli, 'w')
    mli.write(autogen_header("(*", "*)"))
    
    _cinc = sys.argv[4]
    cinc = open(_cinc, 'w')
    cinc.write(autogen_header("/*", "*/"))

    for ty in types:
        #sys.stdout.write(" TYPE    %-20s " % ty.rawname)
        ml.write(gen_ocaml_ml(ty, False))
        ml.write("\n")

        mli.write(gen_ocaml_ml(ty, True))
        mli.write("\n")
        
        if ty.marshal_in():
            cinc.write(gen_c_val(ty))
            cinc.write("\n")
        if ty.marshal_out():
            cinc.write(gen_Val_ocaml(ty))
            cinc.write("\n")
        if functions.has_key(ty.rawname):
            cinc.write(gen_c_stub_prototype(ty, functions[ty.rawname]))
            cinc.write("\n")
        #sys.stdout.write("\n")
    
    ml.write("(* END OF AUTO-GENERATED CODE *)\n")
    ml.close()
    mli.write("(* END OF AUTO-GENERATED CODE *)\n")
    mli.close()
    cinc.close()
