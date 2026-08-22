"""Type-check fixture: ``Field`` metadata and ``Validator`` constraints.

Checked by ``make typecheck-tests`` and ``tests/test_typecheck.py`` against the
shipped ``structtype`` type stubs. Must stay free of ``# type: ignore``.
"""

from collections.abc import Callable
from typing import Annotated, Any

from structtype import (
    UNSET,
    Factory,
    Field,
    NumericValidator,
    Serializer,
    StrValidator,
    Struct,
    UnsetType,
)


class Num(Struct):
    a: Annotated[int, NumericValidator(gt=0, le=10)]
    b: Annotated[float, NumericValidator(ge=0.0, lt=1.0)]
    c: Annotated[str, StrValidator(min_length=1, max_length=10, pattern=r"^[a-z]+$")]
    d: Annotated[int, NumericValidator(multiple_of=2)]


num = Num(a=1, b=0.5, c="abc", d=4)
fa: int = num.a


class Opt(Struct):
    x: int | None | UnsetType = UNSET


opt = Opt()
ox: int | None | UnsetType = opt.x


class WithFactory(Struct):
    items: list[int] = Factory(list)


with_factory = WithFactory()
items: list[int] = with_factory.items

# `Factory(list)` is typed as `Any` (its `__new__` returns `Any` so the
# field-default pattern type-checks); annotate explicitly to reach the
# `.factory` attribute.
factory: Factory = Factory(list)
factory_fn: Callable[[], Any] = factory.factory

# Field attribute access
field = Field(alias="x")
fa_alias: str | None = field.alias
