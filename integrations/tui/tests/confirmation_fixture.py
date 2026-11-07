"""Keep simulated input responsive without changing real confirmation deadlines."""

from contextlib import asynccontextmanager
from unittest.mock import patch

from textual._wait import wait_for_idle


@asynccontextmanager
async def confirmation_input():
    # Pilot can otherwise wait a whole second for CPU idleness after each key
    # or mouse event, unnecessarily consuming the confirmation window.
    async def input_idle(min_sleep=0):
        await wait_for_idle(min_sleep, max_sleep=0.05)
    with patch("textual.app.wait_for_idle", side_effect=input_idle), \
            patch("textual.pilot.wait_for_idle", side_effect=input_idle):
        yield
