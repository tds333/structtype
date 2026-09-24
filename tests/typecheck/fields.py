"""Type-check fixture: ``Field`` metadata and ``Constraint`` constraints.

Checked by ``make typecheck-tests`` and ``tests/test_typecheck.py`` against the
shipped ``structtype`` type stubs. Must stay free of ``# type: ignore``.
"""

from collections.abc import Callable
from typing import Annotated, Any

from structtype import (
    UNSET,
    Constraint,
    Factory,
    Field,
    NumericConstraint,
    Serializer,
    StrConstraint,
    Struct,
    TimezoneConstraint,
    UnsetType,
)


class Num(Struct):
    a: Annotated[int, NumericConstraint(gt=0, le=10)]
    b: Annotated[float, NumericConstraint(ge=0.0, lt=1.0)]
    c: Annotated[str, StrConstraint(min_length=1, max_length=10, pattern=r"^[a-z]+$")]
    d: Annotated[int, NumericConstraint(multiple_of=2)]


num = Num(a=1, b=0.5, c="abc", d=4)
fa: int = num.a

# All four valid bound pairings. Mixing `gt` with `ge` or `lt` with `le` is
# rejected by the overloads in the type stubs (and by the runtime).
nc1 = NumericConstraint(gt=1, lt=5)
nc2 = NumericConstraint(gt=1, le=5)
nc3 = NumericConstraint(ge=1, lt=5)
nc4 = NumericConstraint(ge=1, le=5)
nc5 = NumericConstraint(multiple_of=0.5)
nc6 = NumericConstraint(gt=1, le=5, multiple_of=0.5)
nc7 = NumericConstraint(ge=1.5, le=10.5)

# `tz` is positional-or-keyword at runtime
tz1 = TimezoneConstraint(True)
tz2 = TimezoneConstraint(tz=True)


# `_fn` is a read-only attribute, also on the subclasses. It is positional-only
# on the constructor and the callable follows the `Constraint.__call__`
# protocol: it receives the value and signals failure by raising, so it
# returns `None`.
def check(value: Any) -> None:
    if value is None:
        raise ValueError("none")


constraint = Constraint(check)
cfn: Callable[[Any], None] | None = constraint._fn
nc_fn: Callable[[Any], None] | None = nc1._fn
tz_fn: Callable[[Any], None] | None = tz1._fn


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
