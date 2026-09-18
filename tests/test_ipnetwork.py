import ipaddress
from typing import Annotated, TypedDict

import pytest

import structtype
from structtype import ALL_BUILTIN_TYPES, Serializer, Struct, _inspect as mi
from structtype._json_schema import json_schema as make_schema

IP_CASES = [
    (ipaddress.IPv4Address, "1.2.3.4", "ipv4"),
    (ipaddress.IPv6Address, "::1", "ipv6"),
    (ipaddress.IPv4Network, "1.2.3.0/24", "ipv4network"),
    (ipaddress.IPv6Network, "2001:db8::/32", "ipv6network"),
    (ipaddress.IPv4Interface, "1.2.3.4/24", "ipv4interface"),
    (ipaddress.IPv6Interface, "2001:db8::1/32", "ipv6interface"),
]


def _struct(annotation):
    return type(
        "I_" + annotation.__name__,
        (Struct,),
        {"__annotations__": {"v": annotation}},
    )


@pytest.mark.parametrize(
    "annotation,text,format", IP_CASES, ids=[c[0].__name__ for c in IP_CASES]
)
def test_roundtrip_json(annotation, text, format):
    cls = _struct(annotation)
    value = annotation(text)
    obj = cls(value)
    out = cls.struct_validate_json(obj.struct_dump_json())
    assert type(out.v) is annotation
    assert out.v == value


@pytest.mark.parametrize(
    "annotation,text,format", IP_CASES, ids=[c[0].__name__ for c in IP_CASES]
)
def test_roundtrip_struct_dump(annotation, text, format):
    cls = _struct(annotation)
    value = annotation(text)
    out = cls.struct_validate(cls(value).struct_dump())
    assert type(out.v) is annotation
    assert out.v == value


@pytest.mark.parametrize(
    "annotation,text,format", IP_CASES, ids=[c[0].__name__ for c in IP_CASES]
)
def test_struct_adapter(annotation, text, format):
    adapter = structtype.StructAdapter(annotation)
    value = annotation(text)
    out = adapter.struct_validate_json(adapter.struct_dump_json(value))
    assert type(out) is annotation
    assert out == value


def test_network_decodes_to_declared_class_not_address():
    cls = _struct(ipaddress.IPv4Network)
    out = cls.struct_validate_json(b'{"v":"1.2.3.0/24"}')
    assert type(out.v) is ipaddress.IPv4Network


def test_strict_host_bits_rejected():
    cls = _struct(ipaddress.IPv4Network)
    with pytest.raises(structtype.ValidationError, match="Invalid IPv4 network"):
        cls.struct_validate_json(b'{"v":"192.168.1.1/24"}')
    # bare address becomes a /32 network
    assert cls.struct_validate({"v": "192.168.1.0"}).v == ipaddress.IPv4Network(
        "192.168.1.0/32"
    )


def test_family_and_category_mismatch():
    net4 = _struct(ipaddress.IPv4Network)
    with pytest.raises(structtype.ValidationError, match="Invalid IPv4 network"):
        net4.struct_validate_json(b'{"v":"2001:db8::/32"}')
    iface4 = _struct(ipaddress.IPv4Interface)
    with pytest.raises(structtype.ValidationError, match="Invalid IPv4 interface"):
        iface4.struct_validate_json(b'{"v":"::1/24"}')
    addr4 = _struct(ipaddress.IPv4Address)
    with pytest.raises(structtype.ValidationError, match="Invalid IPv4 address"):
        addr4.struct_validate_json(b'{"v":"1.2.3.4/24"}')


def test_category_strictness_on_instances():
    addr = _struct(ipaddress.IPv4Address)
    # interface is a subclass of address
    addr(ipaddress.IPv4Interface("1.2.3.4/24")).struct_check_types()
    with pytest.raises(structtype.ValidationError):
        addr(ipaddress.IPv4Network("1.2.3.0/24")).struct_check_types()
    net = _struct(ipaddress.IPv4Network)
    with pytest.raises(structtype.ValidationError):
        net(ipaddress.IPv4Address("1.2.3.4")).struct_check_types()


def test_dict_keys():
    cls = type(
        "I_DictKeys",
        (Struct,),
        {"__annotations__": {"v": dict[ipaddress.IPv4Network, int]}},
    )
    key = ipaddress.IPv4Network("1.2.3.0/24")
    out = cls.struct_validate_json(cls({key: 1}).struct_dump_json())
    assert out.v == {key: 1}


def test_typeddict():
    class TD(TypedDict):
        n: ipaddress.IPv6Network

    from structtype import StructAdapter

    adapter = StructAdapter(TD)
    value = ipaddress.IPv6Network("2001:db8::/32")
    out = adapter.struct_validate_json(adapter.struct_dump_json({"n": value}))
    assert out["n"] == value


@pytest.mark.parametrize(
    "annotation,text,format", IP_CASES, ids=[c[0].__name__ for c in IP_CASES]
)
def test_builtin_types_preserves(annotation, text, format):
    cls = _struct(annotation)
    value = annotation(text)
    obj = cls(value)
    assert obj.struct_dump(builtin_types=[annotation])["v"] == value
    assert obj.struct_dump(builtin_types=ALL_BUILTIN_TYPES)["v"] == value


@pytest.mark.parametrize(
    "annotation,text,format", IP_CASES, ids=[c[0].__name__ for c in IP_CASES]
)
def test_type_info(annotation, text, format):
    expected = {
        "ipv4": mi.IPv4AddressType,
        "ipv6": mi.IPv6AddressType,
        "ipv4network": mi.IPv4NetworkType,
        "ipv6network": mi.IPv6NetworkType,
        "ipv4interface": mi.IPv4InterfaceType,
        "ipv6interface": mi.IPv6InterfaceType,
    }[format]
    assert mi.type_info(annotation) == expected()


@pytest.mark.parametrize(
    "annotation,text,format", IP_CASES, ids=[c[0].__name__ for c in IP_CASES]
)
def test_schema(annotation, text, format):
    assert make_schema(annotation) == {"type": "string", "format": format}


def test_serializer_on_ip_class():
    ann = Annotated[
        ipaddress.IPv4Network,
        Serializer(dump=str, load=ipaddress.IPv4Network),
    ]
    cls = type("I_Codec", (Struct,), {"__annotations__": {"v": ann}})
    value = ipaddress.IPv4Network("1.2.3.0/24")
    assert cls.struct_validate_json(cls(value).struct_dump_json()).v == value


def test_user_subclass_is_native():
    class MyIP(ipaddress.IPv4Address):
        pass

    cls = _struct(MyIP)
    out = cls.struct_validate_json(b'{"v":"1.2.3.4"}')
    assert type(out.v) is MyIP
    assert out.v == MyIP("1.2.3.4")
    assert cls(MyIP("1.2.3.4")).struct_dump_json() == b'{"v":"1.2.3.4"}'


def test_union_of_distinct_ip_types_rejected():
    for ann in (
        ipaddress.IPv4Address | ipaddress.IPv4Network,
        ipaddress.IPv4Network | ipaddress.IPv6Network,
    ):
        with pytest.raises(TypeError, match="more than one IP type"):
            cls = type("I_Union", (Struct,), {"__annotations__": {"v": ann}})
            cls.struct_validate({"v": ipaddress.IPv4Address("1.2.3.4")})


def test_optional_ip():
    cls = type(
        "I_Opt",
        (Struct,),
        {"__annotations__": {"v": ipaddress.IPv6Network | None}},
    )
    assert cls(None).struct_dump_json() == b'{"v":null}'
    value = ipaddress.IPv6Network("2001:db8::/32")
    assert cls.struct_validate_json(cls(value).struct_dump_json()).v == value
