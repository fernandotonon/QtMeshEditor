import QtQuick
import QtQuick.Window
import ViewCubeModule 1.0

Window {
    id: root
    width: 50
    height: 50
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: ViewCubeController.visible
    x: ViewCubeController.windowX
    y: ViewCubeController.windowY

    // Internal state
    property string hoveredZone: ""  // face name, corner name, or ""
    property bool isDragging: false
    property real lastMouseX: 0
    property real lastMouseY: 0
    property real pressX: 0
    property real pressY: 0

    Canvas {
        id: cubeCanvas
        anchors.fill: parent
        renderTarget: Canvas.Image

        onPaint: {
            var ctx = getContext("2d");
            ctx.clearRect(0, 0, width, height);

            var qw = ViewCubeController.qw;
            var qx = ViewCubeController.qx;
            var qy = ViewCubeController.qy;
            var qz = ViewCubeController.qz;

            var m = quatToMatrix(qw, -qx, qy, qz);

            var s = 14;
            var cx = width / 2;
            var cy = height / 2;

            // 8 cube vertices
            var verts = [
                [-s, -s, -s], [ s, -s, -s], [ s,  s, -s], [-s,  s, -s],
                [-s, -s,  s], [ s, -s,  s], [ s,  s,  s], [-s,  s,  s]
            ];

            // Rotate and project vertices
            var projected = [];
            for (var i = 0; i < 8; i++) {
                var v = rotatePoint(verts[i], m);
                projected.push({ x: cx + v[0], y: cy - v[1], z: v[2] });
            }

            // 6 faces
            var faces = [
                { idx: [0, 1, 2, 3], name: "Front",  color: [100, 140, 200] },
                { idx: [5, 4, 7, 6], name: "Back",   color: [100, 140, 200] },
                { idx: [1, 5, 6, 2], name: "Right",  color: [130, 160, 210] },
                { idx: [4, 0, 3, 7], name: "Left",   color: [130, 160, 210] },
                { idx: [3, 2, 6, 7], name: "Top",    color: [160, 180, 220] },
                { idx: [4, 5, 1, 0], name: "Bottom", color: [90, 120, 180]  }
            ];

            var faceNormals = [
                [0, 0, -1], [0, 0, 1], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0]
            ];

            var lightDir = normalize([0.3, 0.6, -0.7]);

            var visibleFaces = [];
            for (var fi = 0; fi < faces.length; fi++) {
                var fn = rotatePoint(faceNormals[fi], m);
                if (fn[2] >= 0) continue;

                var depthSum = 0;
                var pts = [];
                for (var pi = 0; pi < 4; pi++) {
                    var p = projected[faces[fi].idx[pi]];
                    pts.push(p);
                    depthSum += p.z;
                }

                var dot = fn[0] * lightDir[0] + fn[1] * lightDir[1] + fn[2] * lightDir[2];
                var brightness = 0.4 + 0.6 * Math.max(0, -dot);

                visibleFaces.push({
                    pts: pts,
                    depth: depthSum / 4,
                    name: faces[fi].name,
                    color: faces[fi].color,
                    brightness: brightness
                });
            }

            visibleFaces.sort(function(a, b) { return b.depth - a.depth; });

            // Draw faces
            for (var di = 0; di < visibleFaces.length; di++) {
                var face = visibleFaces[di];
                var r = Math.round(face.color[0] * face.brightness);
                var g = Math.round(face.color[1] * face.brightness);
                var b = Math.round(face.color[2] * face.brightness);

                var isHovered = (root.hoveredZone === face.name);
                if (isHovered) {
                    r = Math.min(255, r + 50);
                    g = Math.min(255, g + 50);
                    b = Math.min(255, b + 50);
                }

                ctx.beginPath();
                ctx.moveTo(face.pts[0].x, face.pts[0].y);
                for (var k = 1; k < 4; k++)
                    ctx.lineTo(face.pts[k].x, face.pts[k].y);
                ctx.closePath();
                ctx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
                ctx.fill();
                ctx.strokeStyle = "rgba(255,255,255,0.3)";
                ctx.lineWidth = 1;
                ctx.stroke();

                // Face label
                var centerX = (face.pts[0].x + face.pts[1].x + face.pts[2].x + face.pts[3].x) / 4;
                var centerY = (face.pts[0].y + face.pts[1].y + face.pts[2].y + face.pts[3].y) / 4;
                ctx.font = isHovered ? "bold 7px sans-serif" : "6px sans-serif";
                ctx.fillStyle = "white";
                ctx.textAlign = "center";
                ctx.textBaseline = "middle";
                ctx.fillText(faceLabel(face.name), centerX, centerY);
            }

            // Draw corners (small dots at visible vertices)
            var cornerDefs = getCornerDefs();
            for (var ci = 0; ci < cornerDefs.length; ci++) {
                var corner = cornerDefs[ci];
                var cp = projected[corner.vertIdx];

                // Only draw if vertex is on the front-facing side
                if (cp.z > 0) continue;

                var cornerRadius = 3;
                var cornerHovered = (root.hoveredZone === corner.name);

                ctx.beginPath();
                ctx.arc(cp.x, cp.y, cornerHovered ? cornerRadius + 2 : cornerRadius, 0, 2 * Math.PI);
                ctx.fillStyle = cornerHovered ? "rgba(255, 255, 255, 0.9)" : "rgba(200, 220, 255, 0.6)";
                ctx.fill();
                ctx.strokeStyle = "rgba(255, 255, 255, 0.8)";
                ctx.lineWidth = 1;
                ctx.stroke();
            }

            // Draw axis indicator
            drawAxes(ctx, m, 10, height - 12);
        }

        // --- Helper functions ---

        function quatToMatrix(w, x, y, z) {
            var xx = x*x, yy = y*y, zz = z*z;
            var xy = x*y, xz = x*z, yz = y*z;
            var wx = w*x, wy = w*y, wz = w*z;
            return [
                1 - 2*(yy+zz), 2*(xy-wz),     2*(xz+wy),
                2*(xy+wz),     1 - 2*(xx+zz), 2*(yz-wx),
                2*(xz-wy),     2*(yz+wx),     1 - 2*(xx+yy)
            ];
        }

        function rotatePoint(p, m) {
            return [
                m[0]*p[0] + m[1]*p[1] + m[2]*p[2],
                m[3]*p[0] + m[4]*p[1] + m[5]*p[2],
                m[6]*p[0] + m[7]*p[1] + m[8]*p[2]
            ];
        }

        function normalize(v) {
            var len = Math.sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]);
            if (len < 0.0001) return [0, 0, 0];
            return [v[0]/len, v[1]/len, v[2]/len];
        }

        function faceLabel(name) {
            switch (name) {
                case "Front":  return "F";
                case "Back":   return "Bk";
                case "Left":   return "L";
                case "Right":  return "R";
                case "Top":    return "T";
                case "Bottom": return "Bt";
            }
            return "";
        }

        function drawAxes(ctx, m, ox, oy) {
            var axisLen = 8;
            var axes = [
                { dir: [1, 0, 0], color: "red",     label: "X" },
                { dir: [0, 1, 0], color: "green",   label: "Y" },
                { dir: [0, 0, -1], color: "#4488ff", label: "Z" }
            ];
            for (var i = 0; i < axes.length; i++) {
                var tip = rotatePoint(axes[i].dir, m);
                var ex = ox + tip[0] * axisLen;
                var ey = oy - tip[1] * axisLen;
                ctx.beginPath();
                ctx.moveTo(ox, oy);
                ctx.lineTo(ex, ey);
                ctx.strokeStyle = axes[i].color;
                ctx.lineWidth = 1.5;
                ctx.stroke();
                ctx.font = "bold 6px sans-serif";
                ctx.fillStyle = axes[i].color;
                ctx.textAlign = "center";
                ctx.textBaseline = "middle";
                ctx.fillText(axes[i].label, ex + (tip[0] > 0 ? 4 : -4), ey - (tip[1] > 0 ? 4 : -4));
            }
        }

        function pointInPolygon(px, py, pts) {
            var inside = false;
            for (var i = 0, j = pts.length - 1; i < pts.length; j = i++) {
                var xi = pts[i].x, yi = pts[i].y;
                var xj = pts[j].x, yj = pts[j].y;
                if (((yi > py) !== (yj > py)) &&
                    (px < (xj - xi) * (py - yi) / (yj - yi) + xi))
                    inside = !inside;
            }
            return inside;
        }

        // Corner definitions: vertex index and the "look-from" direction
        // Vertex layout:
        //   0:(-s,-s,-s) 1:(+s,-s,-s) 2:(+s,+s,-s) 3:(-s,+s,-s)
        //   4:(-s,-s,+s) 5:(+s,-s,+s) 6:(+s,+s,+s) 7:(-s,+s,+s)
        function getCornerDefs() {
            return [
                { vertIdx: 0, name: "FrontBottomLeft",  dir: [ 1, -1, -1] },
                { vertIdx: 1, name: "FrontBottomRight", dir: [-1, -1, -1] },
                { vertIdx: 2, name: "FrontTopRight",    dir: [-1,  1, -1] },
                { vertIdx: 3, name: "FrontTopLeft",     dir: [ 1,  1, -1] },
                { vertIdx: 4, name: "BackBottomLeft",   dir: [ 1, -1,  1] },
                { vertIdx: 5, name: "BackBottomRight",  dir: [-1, -1,  1] },
                { vertIdx: 6, name: "BackTopRight",     dir: [-1,  1,  1] },
                { vertIdx: 7, name: "BackTopLeft",      dir: [ 1,  1,  1] }
            ];
        }

        // Edge definitions: two vertex indices and the "look-from" direction
        // Direction is derived from the edge midpoint with X negated (same convention as corners)
        function getEdgeDefs() {
            return [
                // Front face edges
                { vertA: 0, vertB: 1, name: "EdgeFrontBottom",  dir: [ 0, -1, -1] },
                { vertA: 1, vertB: 2, name: "EdgeFrontRight",   dir: [-1,  0, -1] },
                { vertA: 2, vertB: 3, name: "EdgeFrontTop",     dir: [ 0,  1, -1] },
                { vertA: 3, vertB: 0, name: "EdgeFrontLeft",    dir: [ 1,  0, -1] },
                // Back face edges
                { vertA: 4, vertB: 5, name: "EdgeBackBottom",   dir: [ 0, -1,  1] },
                { vertA: 5, vertB: 6, name: "EdgeBackRight",    dir: [-1,  0,  1] },
                { vertA: 6, vertB: 7, name: "EdgeBackTop",      dir: [ 0,  1,  1] },
                { vertA: 7, vertB: 4, name: "EdgeBackLeft",     dir: [ 1,  0,  1] },
                // Connecting edges (front-to-back)
                { vertA: 0, vertB: 4, name: "EdgeLeftBottom",   dir: [ 1, -1,  0] },
                { vertA: 1, vertB: 5, name: "EdgeRightBottom",  dir: [-1, -1,  0] },
                { vertA: 2, vertB: 6, name: "EdgeRightTop",     dir: [-1,  1,  0] },
                { vertA: 3, vertB: 7, name: "EdgeLeftTop",      dir: [ 1,  1,  0] }
            ];
        }
    }

    // Repaint when orientation changes
    Connections {
        target: ViewCubeController
        function onOrientationChanged() { cubeCanvas.requestPaint(); }
        function onVisibilityChanged() { cubeCanvas.requestPaint(); }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.isDragging ? Qt.ClosedHandCursor
                   : root.hoveredZone !== "" ? Qt.PointingHandCursor
                   : Qt.OpenHandCursor

        onPressed: function(mouse) {
            root.pressX = mouse.x;
            root.pressY = mouse.y;
            root.lastMouseX = mouse.x;
            root.lastMouseY = mouse.y;
            root.isDragging = false;
        }

        onPositionChanged: function(mouse) {
            if (mouse.buttons & Qt.LeftButton) {
                var dx = mouse.x - root.lastMouseX;
                var dy = mouse.y - root.lastMouseY;

                // Start drag after a small threshold to distinguish from click
                if (!root.isDragging) {
                    var totalDist = Math.abs(mouse.x - root.pressX) + Math.abs(mouse.y - root.pressY);
                    if (totalDist > 3)
                        root.isDragging = true;
                }

                if (root.isDragging) {
                    ViewCubeController.rotateByDelta(dx, dy);
                }
                root.lastMouseX = mouse.x;
                root.lastMouseY = mouse.y;
            } else {
                // Hover hit-test
                root.hoveredZone = hitTest(mouse.x, mouse.y);
                cubeCanvas.requestPaint();
            }
        }

        onReleased: function(mouse) {
            if (!root.isDragging) {
                // It was a click — hit-test and snap
                var zone = hitTest(mouse.x, mouse.y);
                if (zone !== "")
                    handleClick(zone);
            }
            root.isDragging = false;
        }

        onExited: {
            root.hoveredZone = "";
            cubeCanvas.requestPaint();
        }

        function handleClick(zone) {
            // Check if it's a corner
            var cornerDefs = cubeCanvas.getCornerDefs();
            for (var i = 0; i < cornerDefs.length; i++) {
                if (cornerDefs[i].name === zone) {
                    var d = cornerDefs[i].dir;
                    ViewCubeController.snapToDirection(d[0], d[1], d[2]);
                    return;
                }
            }
            // Check if it's an edge
            var edgeDefs = cubeCanvas.getEdgeDefs();
            for (var j = 0; j < edgeDefs.length; j++) {
                if (edgeDefs[j].name === zone) {
                    var ed = edgeDefs[j].dir;
                    ViewCubeController.snapToDirection(ed[0], ed[1], ed[2]);
                    return;
                }
            }
            // Otherwise it's a face
            ViewCubeController.snapToView(zone);
        }

        function hitTest(mx, my) {
            var qw = ViewCubeController.qw;
            var qx = ViewCubeController.qx;
            var qy = ViewCubeController.qy;
            var qz = ViewCubeController.qz;
            var m = cubeCanvas.quatToMatrix(qw, -qx, qy, qz);

            var s = 14;
            var cx = cubeCanvas.width / 2;
            var cy = cubeCanvas.height / 2;

            var verts = [
                [-s, -s, -s], [ s, -s, -s], [ s,  s, -s], [-s,  s, -s],
                [-s, -s,  s], [ s, -s,  s], [ s,  s,  s], [-s,  s,  s]
            ];

            var projected = [];
            for (var i = 0; i < 8; i++) {
                var v = cubeCanvas.rotatePoint(verts[i], m);
                projected.push({ x: cx + v[0], y: cy - v[1], z: v[2] });
            }

            // Check corners first (they're smaller targets, prioritize them)
            var cornerDefs = cubeCanvas.getCornerDefs();
            var cornerHitRadius = 8;
            var bestCorner = "";
            var bestCornerDepth = Infinity;
            for (var ci = 0; ci < cornerDefs.length; ci++) {
                var corner = cornerDefs[ci];
                var cp = projected[corner.vertIdx];
                if (cp.z > 0) continue; // behind

                var dist = Math.sqrt((mx - cp.x) * (mx - cp.x) + (my - cp.y) * (my - cp.y));
                if (dist <= cornerHitRadius && cp.z < bestCornerDepth) {
                    bestCorner = corner.name;
                    bestCornerDepth = cp.z;
                }
            }
            if (bestCorner !== "") return bestCorner;

            // Check edges (midpoint of each edge's two vertices)
            var edgeDefs = cubeCanvas.getEdgeDefs();
            var edgeHitRadius = 6;
            var bestEdge = "";
            var bestEdgeDepth = Infinity;
            for (var ei = 0; ei < edgeDefs.length; ei++) {
                var edge = edgeDefs[ei];
                var pa = projected[edge.vertA];
                var pb = projected[edge.vertB];
                var midX = (pa.x + pb.x) / 2;
                var midY = (pa.y + pb.y) / 2;
                var midZ = (pa.z + pb.z) / 2;
                if (midZ > 0) continue; // behind

                var edist = Math.sqrt((mx - midX) * (mx - midX) + (my - midY) * (my - midY));
                if (edist <= edgeHitRadius && midZ < bestEdgeDepth) {
                    bestEdge = edge.name;
                    bestEdgeDepth = midZ;
                }
            }
            if (bestEdge !== "") return bestEdge;

            // Check faces
            var faces = [
                { idx: [0, 1, 2, 3], name: "Front"  },
                { idx: [5, 4, 7, 6], name: "Back"   },
                { idx: [1, 5, 6, 2], name: "Right"  },
                { idx: [4, 0, 3, 7], name: "Left"   },
                { idx: [3, 2, 6, 7], name: "Top"    },
                { idx: [4, 5, 1, 0], name: "Bottom" }
            ];
            var faceNormals = [
                [0, 0, -1], [0, 0, 1], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0]
            ];

            var candidates = [];
            for (var fi = 0; fi < faces.length; fi++) {
                var fn = cubeCanvas.rotatePoint(faceNormals[fi], m);
                if (fn[2] >= 0) continue;

                var pts = [];
                var depthSum = 0;
                for (var pi = 0; pi < 4; pi++) {
                    var p = projected[faces[fi].idx[pi]];
                    pts.push(p);
                    depthSum += p.z;
                }

                if (cubeCanvas.pointInPolygon(mx, my, pts))
                    candidates.push({ name: faces[fi].name, depth: depthSum / 4 });
            }

            if (candidates.length === 0) return "";
            candidates.sort(function(a, b) { return a.depth - b.depth; });
            return candidates[0].name;
        }
    }
}
