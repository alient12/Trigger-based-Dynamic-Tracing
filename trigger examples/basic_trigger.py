#!/usr/bin/env python3
"""
basic_trigger.py
Demonstrates a basic trigger-based dynamic tracing example.
A trigger activates when a specific keyword is found in an event string,
and deactivates when the keyword is absent.
"""

import sys


class Tracer:
    def __init__(self):
        self._log = []
        self._active = False

    def start(self, label):
        self._active = True
        self._log.append(f"[START:{label}]")

    def stop(self, label):
        self._active = False
        self._log.append(f"[STOP:{label}]")

    def record(self, event):
        if self._active:
            self._log.append(event)

    def dump(self):
        return " ".join(self._log)


class KeywordTrigger:
    def __init__(self, keyword, label, tracer):
        self.keyword = keyword
        self.label = label
        self.tracer = tracer
        self._active = False

    def fire(self, event):
        if self.keyword in event:
            if not self._active:
                self.tracer.start(self.label)
                self._active = True
            self.tracer.record(event)
        else:
            if self._active:
                self.tracer.stop(self.label)
                self._active = False


def main():
    tracer = Tracer()
    trigger = KeywordTrigger("ERROR", "error-trace", tracer)

    events = [
        "INFO  server started",
        "INFO  request received",
        "ERROR disk full",
        "ERROR retry attempt 1",
        "ERROR retry attempt 2",
        "INFO  disk space freed",
        "INFO  request completed",
    ]

    for event in events:
        trigger.fire(event)
        print(f"event: {event}")

    print("\nTrace output:")
    print(tracer.dump())


if __name__ == "__main__":
    main()
