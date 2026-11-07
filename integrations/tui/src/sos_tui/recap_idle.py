"""One recap after five continuous idle minutes in this TUI process."""


class RecapIdle:
    DELAY_SECONDS = 5 * 60

    def __init__(self) -> None:
        self.since: float | None = None
        self.requested = False

    def activity(self) -> None:
        self.since = None
        self.requested = False

    def pause(self) -> None:
        self.since = None

    def mark_requested(self) -> None:
        # Manual recaps also satisfy this pause. Failed requests are not retried
        # on a timer; the user can explicitly request another recap.
        self.pause()
        self.requested = True

    def due(self, now: float) -> bool:
        if self.requested:
            return False
        if self.since is None:
            self.since = now
        return now - self.since >= self.DELAY_SECONDS
