#!/usr/bin/env python3
"""Drive a Clarion QY7 along a JSON or GPX route over QMP."""
import argparse
import json
import math
import os
import time
import xml.etree.ElementTree as ET

from qmp import Qmp

R_EARTH = 6371000.0


def distance(a, b):
    lat1, lat2 = math.radians(a[0]), math.radians(b[0])
    dlat = lat2 - lat1
    dlon = math.radians(b[1] - a[1])
    x = math.sin(dlat / 2) ** 2
    x += math.cos(lat1) * math.cos(lat2) * math.sin(dlon / 2) ** 2
    return 2 * R_EARTH * math.asin(min(1.0, math.sqrt(x)))


def bearing(a, b):
    lat1, lat2 = math.radians(a[0]), math.radians(b[0])
    dlon = math.radians(b[1] - a[1])
    y = math.sin(dlon) * math.cos(lat2)
    x = math.cos(lat1) * math.sin(lat2)
    x -= math.sin(lat1) * math.cos(lat2) * math.cos(dlon)
    return math.degrees(math.atan2(y, x)) % 360.0


def destination(start, course, distance_m):
    lat1, lon1 = map(math.radians, start)
    angle = math.radians(course)
    arc = distance_m / R_EARTH
    lat2 = math.asin(math.sin(lat1) * math.cos(arc) +
                     math.cos(lat1) * math.sin(arc) * math.cos(angle))
    lon2 = lon1 + math.atan2(math.sin(angle) * math.sin(arc) * math.cos(lat1),
                             math.cos(arc) - math.sin(lat1) * math.sin(lat2))
    lon = (math.degrees(lon2) + 540.0) % 360.0 - 180.0
    return math.degrees(lat2), lon


def local_points(data, default_speed):
    points = data.get("points", [])
    if len(points) < 2:
        raise ValueError("JSON route needs at least two points")
    if any("east_m" not in p or "north_m" not in p for p in points):
        raise ValueError("JSON points need east_m and north_m")
    segments = []
    for a, b in zip(points, points[1:]):
        east = float(b["east_m"]) - float(a["east_m"])
        north = float(b["north_m"]) - float(a["north_m"])
        length = math.hypot(east, north)
        if length <= 0:
            raise ValueError("route segments must have positive length")
        course = math.degrees(math.atan2(east, north)) % 360.0
        segments.append((length, course,
                         float(a.get("speed_kmh", default_speed))))
    heading = float(data.get("initial_heading", segments[0][1])) % 360.0
    return heading, segments


def gpx_points(path, default_speed):
    root = ET.parse(path).getroot()
    points = []
    for node in root.iter():
        if node.tag.rsplit("}", 1)[-1] != "trkpt":
            continue
        lat, lon = float(node.attrib["lat"]), float(node.attrib["lon"])
        stamp = None
        for child in node:
            if child.tag.rsplit("}", 1)[-1] == "time":
                stamp = child.text
                break
        points.append(((lat, lon), stamp))
    if len(points) < 2:
        raise ValueError("GPX route needs at least two track points")
    segments = []
    for (a, t0), (b, t1) in zip(points, points[1:]):
        length = distance(a, b)
        if length <= 0:
            continue
        speed = default_speed
        if t0 and t1:
            from datetime import datetime
            end_time = datetime.fromisoformat(t1.replace("Z", "+00:00"))
            start_time = datetime.fromisoformat(t0.replace("Z", "+00:00"))
            elapsed = (end_time - start_time).total_seconds()
            if elapsed > 0:
                speed = length / elapsed * 3.6
        segments.append((length, bearing(a, b), speed))
    if not segments:
        raise ValueError("GPX route has no nonzero-length segments")
    return segments[0][1], segments


class Route:
    def __init__(self, heading, segments):
        self.heading = heading % 360.0
        self.segments = [(length, course % 360.0, round(speed))
                         for length, course, speed in segments]
        if any(speed < 1 or speed > 1000
               for _, _, speed in self.segments):
            raise ValueError("segment speeds must round to [1, 1000] km/h")
        self.ends = [0.0]
        for length, _, _ in segments:
            self.ends.append(self.ends[-1] + length)
        self.total = self.ends[-1]

    @classmethod
    def load(cls, path, default_speed):
        if path.lower().endswith(".gpx"):
            heading, segments = gpx_points(path, default_speed)
        else:
            with open(path, encoding="utf-8") as route_file:
                data = json.load(route_file)
                heading, segments = local_points(data, default_speed)
        return cls(heading, segments)

    def at(self, distance_m, start):
        distance_m = min(max(distance_m, 0.0), self.total)
        seg = len(self.segments) - 1
        for i, end in enumerate(self.ends[1:]):
            if distance_m <= end:
                seg = i
                break
        pos = start
        for i in range(seg):
            length, course, _ = self.segments[i]
            pos = destination(pos, course, length)
        length, course, speed = self.segments[seg]
        pos = destination(pos, course, distance_m - self.ends[seg])
        return pos, seg, speed, course

    @staticmethod
    def turn_delta(target, current):
        return (target - current + 180.0) % 360.0 - 180.0


class Session:
    def __init__(self, path):
        self.qmp = Qmp(path)

    def prop(self, name, value):
        reply = self.qmp.qom_set(name, value)
        if "error" in reply:
            raise RuntimeError(reply)

    def getprop(self, name):
        reply = self.qmp.qom_get(name)
        if "error" in reply:
            raise RuntimeError(reply)
        return reply["return"]

    def shot(self, path):
        reply = self.qmp.screendump(path)
        if "error" in reply:
            raise RuntimeError(reply)


def run(args):
    if args.speed <= 0 or not math.isfinite(args.speed):
        raise ValueError("--speed must be a finite positive value")
    route = Route.load(args.route, args.speed)
    session = Session(args.socket)
    start = (float(session.getprop("gps-lat")),
             float(session.getprop("gps-lon")))
    os.makedirs(args.out, exist_ok=True)
    marks = sorted(float(mark) for mark in args.marks.split(",")
                   if mark.strip())
    pending = list(marks)
    session.prop("speed-kmh", 0)
    session.prop("yaw-rate", 0)
    start_odometer = float(session.getprop("odometer-m"))
    segment = 0
    distance_m = 0.0
    speed = route.segments[0][2]
    turn = Route.turn_delta(route.segments[0][1], route.heading)
    yaw = math.copysign(20.0, turn) if abs(turn) >= 0.5 else 0.0
    yaw_until = (speed / 3.6 * abs(turn) / 20.0) if yaw else None
    session.prop("gps-course", route.segments[0][1])
    session.prop("gps-speed-kmh", speed)
    session.prop("speed-kmh", round(speed))
    session.prop("yaw-rate", yaw)

    log_path = os.path.join(args.out, "track.jsonl")
    with open(log_path, "w", encoding="utf-8") as log:
        log.write(json.dumps({"total_m": round(route.total, 2),
                              "start_odometer_m": start_odometer,
                              "poll_s": args.poll}) + "\n")
        for mark in pending[:]:
            if mark <= 0:
                session.shot(os.path.join(args.out, "f%05.1f.png" % mark))
                pending.remove(mark)
        while distance_m < route.total:
            odometer = float(session.getprop("odometer-m"))
            distance_m = max(0.0, odometer - start_odometer)
            pos, next_segment, speed, course = route.at(distance_m, start)
            if next_segment != segment:
                previous_course = route.segments[segment][1]
                turn = Route.turn_delta(course, previous_course)
                segment = next_segment
                yaw = math.copysign(20.0, turn) if abs(turn) >= 0.5 else 0.0
                yaw_until = (distance_m + speed / 3.6 * abs(turn) / 20.0
                             if yaw else None)
                session.prop("speed-kmh", round(speed))
                session.prop("yaw-rate", yaw)
            if yaw and yaw_until is not None and distance_m >= yaw_until:
                yaw = 0.0
                yaw_until = None
                session.prop("yaw-rate", 0)
            session.prop("gps-lat", str(pos[0]))
            session.prop("gps-lon", str(pos[1]))
            session.prop("gps-speed-kmh", speed)
            session.prop("gps-course", course)
            log.write(json.dumps({"odometer_m": round(odometer, 3),
                                  "distance_m": round(distance_m, 3),
                                  "segment": segment,
                                  "speed_kmh": speed,
                                  "course_deg": course,
                                  "yaw_rate_deg_s": yaw}) + "\n")
            log.flush()
            for mark in pending[:]:
                if distance_m >= mark:
                    session.shot(os.path.join(args.out, "f%05.1f.png" % mark))
                    pending.remove(mark)
            time.sleep(args.poll)

    session.prop("speed-kmh", 0)
    session.prop("gps-speed-kmh", 0)
    session.prop("yaw-rate", 0)
    for mark in pending:
        session.shot(os.path.join(args.out, "f%05.1f.png" % mark))
    print("done, distance %.1f m" % distance_m)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("socket", help="QMP Unix socket")
    parser.add_argument("--route", required=True, help="JSON or GPX route")
    parser.add_argument("--speed", type=float, default=40.0,
                        help="speed in km/h when a segment has no speed")
    parser.add_argument("--marks", default="",
                        help="route distances for map screenshots, in meters")
    parser.add_argument("--out", default="route-output",
                        help="directory for screenshots and track log")
    parser.add_argument("--poll", type=float, default=0.1,
                        help="odometer polling interval in seconds")
    run(parser.parse_args())


if __name__ == "__main__":
    main()
