"""Bounded synchronization for deferred Textual effects, not fixed-delay sleeps."""

from time import monotonic


async def wait_until(pilot, predicate, description, *, timeout=2.0):
    deadline = monotonic() + timeout
    while not predicate():
        if monotonic() >= deadline:
            raise AssertionError(f"Timed out waiting for {description}")
        await pilot.pause(0.01)
