#!/usr/bin/env python3
"""
multi_trigger.py
Demonstrates multiple simultaneous triggers each watching different event prefixes.
Each trigger independently manages its own tracing window.
"""

class Tracer:
    def __init__(self, name):
        self.name = name
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
        return " ".join(self._log) if self._log else "(empty)"


class PrefixTrigger:
    def __init__(self, prefix, label):
        self.prefix = prefix
        self.label = label
        self.tracer = Tracer(label)
        self._active = False

    def fire(self, event):
        if event.startswith(self.prefix):
            if not self._active:
                self.tracer.start(self.label)
                self._active = True
            self.tracer.record(event)
        else:
            if self._active:
                self.tracer.stop(self.label)
                self._active = False


def main():
    triggers = [
        PrefixTrigger("net:", "network"),
        PrefixTrigger("fs:", "filesystem"),
        PrefixTrigger("db:", "database"),
    ]

    events = [
        "net:connect host=db.example.com",
        "db:query SELECT * FROM users",
        "db:result rows=42",
        "net:recv bytes=1024",
        "fs:open path=/tmp/cache.tmp",
        "fs:write bytes=512",
        "db:commit",
        "fs:close path=/tmp/cache.tmp",
        "net:disconnect host=db.example.com",
    ]

    for event in events:
        for trigger in triggers:
            trigger.fire(event)

    print("=== Trace Results ===")
    for trigger in triggers:
        print(f"\n[{trigger.label}]")
        print(trigger.tracer.dump())


if __name__ == "__main__":
    main()
