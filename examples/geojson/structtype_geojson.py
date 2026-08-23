from __future__ import annotations

import structtype
from structtype import StructConfig

Position = tuple[float, float]


# All 9 GeoJSON types share the same configuration: they make use of a `type`
# field to disambiguate between types when decoding. Define it once on a base
# class — subclasses inherit the configuration automatically.
class TaggedBase(structtype.Struct):
    struct_config = StructConfig(tag=True)


# Define the 7 standard Geometry types.
class Point(TaggedBase):
    coordinates: Position


class MultiPoint(TaggedBase):
    coordinates: list[Position]


class LineString(TaggedBase):
    coordinates: list[Position]


class MultiLineString(TaggedBase):
    coordinates: list[list[Position]]


class Polygon(TaggedBase):
    coordinates: list[list[Position]]


class MultiPolygon(TaggedBase):
    coordinates: list[list[list[Position]]]


class GeometryCollection(TaggedBase):
    geometries: list[Geometry]


Geometry = (
    Point
    | MultiPoint
    | LineString
    | MultiLineString
    | Polygon
    | MultiPolygon
    | GeometryCollection
)


# Define the two Feature types
class Feature(TaggedBase):
    geometry: Geometry | None = None
    properties: dict | None = None
    id: str | int | None = None


class FeatureCollection(TaggedBase):
    features: list[Feature]


# A union of all 9 GeoJSON types
GeoJSON = Geometry | Feature | FeatureCollection


# Create a decoder and an encoder to use for decoding & encoding GeoJSON types
loads = structtype.StructAdapter(GeoJSON).struct_validate_json
dumps = structtype.StructAdapter(GeoJSON).struct_dump_json
