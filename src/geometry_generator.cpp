#include "../include/geometry_generator.h"
#include <stdexcept>

GeometryGenerator::GeometryGenerator() {
    m_vertexData = nullptr;
    m_indexData = nullptr;

    m_state.vertexPointer = 0;
    m_state.indexPointer = 0;

    m_indexBufferSize = 0;
    m_vertexBufferSize = 0;

    m_state.subshapeVertexPointer = 0;
}

GeometryGenerator::~GeometryGenerator() {
    destroy();
}

void GeometryGenerator::initialize(int vertexBufferSize, int indexBufferSize) {
    if (vertexBufferSize <= 0 || indexBufferSize <= 0) throw std::invalid_argument("Geometry capacity must be positive");
    destroy();
    m_vertexData = new desktop::Vertex[vertexBufferSize];
    m_indexData = new unsigned short[indexBufferSize];

    m_vertexBufferSize = vertexBufferSize;
    m_indexBufferSize = indexBufferSize;
}

void GeometryGenerator::destroy() {
    delete[] m_vertexData;
    delete[] m_indexData;
    m_vertexData = nullptr;
    m_indexData = nullptr;
    m_vertexBufferSize = m_indexBufferSize = 0;
    reset();
}

void GeometryGenerator::reset() {
    m_state.vertexPointer = 0;
    m_state.indexPointer = 0;
    m_state.subshapeVertexPointer = 0;
}

bool GeometryGenerator::generateFilledCircle(
    const desktop::Vector &normal,
    const desktop::Vector &center,
    float radius,
    float maxEdgeLength)
{
    // edge_length = (sin(theta) * radius) * 2
    // theta = arcsin(edge_length / (2 * radius))

    const float angle = ::asinf(maxEdgeLength / (2 * radius));
    const float steps = desktop::math::Constants::TWO_PI / angle;

    int wholeSteps = (int)::ceilf(steps);
    wholeSteps = (wholeSteps < 3)
        ? 3
        : wholeSteps;

    return generateFilledFanPolygon(
        normal,
        findOrthogonal(normal),
        center,
        radius,
        0.0f,
        wholeSteps);
}

bool GeometryGenerator::generateFilledFanPolygon(
    const desktop::Vector &normal,
    const desktop::Vector &up,
    const desktop::Vector &center,
    float radius,
    float rotation,
    int segmentCount)
{
    startSubshape();

    const int vertexCount = 1 + segmentCount;
    const int faceCount = segmentCount;
    const int indexCount = faceCount * 3;

    if (vertexCount + m_state.vertexPointer > m_vertexBufferSize ||
        indexCount + m_state.indexPointer > m_indexBufferSize)
    {
        return false;
    }

    // Generate center vertex
    desktop::Vertex *centerVertex = writeVertex();
    centerVertex->Normal = desktop::math::GetVector4(normal);
    centerVertex->Pos = desktop::math::GetVector4(center);
    centerVertex->TexCoord = desktop::Vector2(0.5f, 0.5f);

    const float angleStep = desktop::math::Constants::TWO_PI / segmentCount;

    const desktop::Vector right = desktop::math::Cross(up, normal);
    desktop::Matrix T = desktop::math::LoadMatrix(
        right,
        up,
        normal,
        desktop::math::ExtendVector(center)
    );
    T = desktop::math::Transpose(T);

    for (int i = 0; i < segmentCount; ++i) {
        const float angle0 = angleStep * i + rotation;
        const float x0 = ::cosf(angle0);
        const float y0 = ::sinf(angle0);

        const desktop::Vector pos = desktop::math::LoadVector(x0 * radius, y0 * radius, 0.0f, 1.0f);

        desktop::Vertex *newVertex = writeVertex();
        newVertex->Normal = normal;
        newVertex->Pos = desktop::math::MatMult(T, pos);
        newVertex->TexCoord = desktop::Vector2(0.5f * x0 + 0.5f, 0.5f * y0 + 0.5f);
    }

    for (int i = 0; i < segmentCount; ++i) {
        writeFace(0, i + 1, 1 + ((i + 1) % segmentCount));
    }

    return true;
}

bool GeometryGenerator::generateLineRing(
    const LineRingParameters &params)
{
    // edge_length = (sin(theta) * radius) * 2
    // theta = arcsin(edge_length / (2 * radius))

    startSubshape();

    const float actualStartAngle = params.startAngle - params.taperTail;
    const float actualEndAngle = params.endAngle + params.taperTail;

    const float maxOuterRadius = params.radius + (params.patternHeight / 2);

    const float angle = ::asinf(params.maxEdgeLength / (2 * maxOuterRadius));
    const float steps = (actualEndAngle - actualStartAngle) / angle;

    int segmentCount = (int)::ceilf(steps);
    segmentCount = (segmentCount < 3)
        ? 3
        : segmentCount;

    const int vertexCount = (segmentCount + 1) * 2;
    const int faceCount = segmentCount * 2;
    const int indexCount = faceCount * 3;

    const desktop::Vector up = findOrthogonal(params.normal);

    if (vertexCount + m_state.vertexPointer > m_vertexBufferSize ||
        indexCount + m_state.indexPointer > m_indexBufferSize)
    {
        return false;
    }

    // Generate center vertex
    const float angleStep = (actualEndAngle - actualStartAngle) / segmentCount;

    const desktop::Vector right = desktop::math::Cross(up, params.normal);
    desktop::Matrix T = desktop::math::LoadMatrix(
        right,
        up,
        params.normal,
        desktop::math::ExtendVector(params.center)
    );
    T = desktop::math::Transpose(T);

    for (int i = 0; i <= segmentCount; ++i) {
        float angle0 = angleStep * i + actualStartAngle;
        const float x0 = ::cosf(angle0);
        const float y0 = ::sinf(angle0);

        if (angle0 >= actualEndAngle) angle0 = actualEndAngle;
        else if (angle0 <= actualStartAngle) angle0 = actualStartAngle;

        float taper = 1.0f;
        if (params.taperTail != 0) {
            if (angle0 >= actualStartAngle && angle0 < params.startAngle) {
                taper = (angle0 - actualStartAngle) / params.taperTail;
            }
            else if (angle0 > params.endAngle && angle0 <= actualEndAngle) {
                taper = 1.0f - (angle0 - params.endAngle) / params.taperTail;
            }
        }

        const float innerRadius = params.radius - (params.patternHeight / 2) * taper;
        const float outerRadius = params.radius + (params.patternHeight / 2) * taper;

        const desktop::Vector outerPos =
            desktop::math::LoadVector(x0 * outerRadius, y0 * outerRadius, 0.0f, 1.0f);
        const desktop::Vector innerPos =
            desktop::math::LoadVector(x0 * innerRadius, y0 * innerRadius, 0.0f, 1.0f);

        const float s =
            params.textureOffset +
            angle0 * params.radius / (params.patternHeight * params.textureWidthHeightRatio);

        desktop::Vertex *outerVertex = writeVertex();
        outerVertex->Normal = params.normal;
        outerVertex->Pos = desktop::math::MatMult(T, outerPos);
        outerVertex->TexCoord = desktop::Vector2(s, 1.0f);

        desktop::Vertex *innerVertex = writeVertex();
        innerVertex->Normal = params.normal;
        innerVertex->Pos = desktop::math::MatMult(T, innerPos);
        innerVertex->TexCoord = desktop::Vector2(s, 0.0f);
    }

#define OUTER(i) (((i)) * 2)
#define INNER(i) (((i)) * 2 + 1)

    for (int i = 0; i < segmentCount; ++i) {
        writeFace(INNER(i), OUTER(i + 1), INNER(i + 1));
        writeFace(INNER(i), OUTER(i), OUTER(i + 1));
    }

    return true;
}

bool GeometryGenerator::generateLineRingBalanced(
    const LineRingParameters &params)
{
    const float midpoint = (params.startAngle + params.endAngle) / 2.0f;
    const float offset =
        params.textureOffset -
        (midpoint * params.radius) / (params.patternHeight * params.textureWidthHeightRatio);

    LineRingParameters augmentedParams = params;
    augmentedParams.textureOffset = offset;

    generateLineRing(augmentedParams);

    return true;
}

bool GeometryGenerator::generateLine(
    const LineParameters &params)
{
    startSubshape();

    const desktop::Vector tangent = desktop::math::Normalize(desktop::math::Sub(params.end, params.start));
    const desktop::Vector right = desktop::math::Cross(params.normal, tangent);

    const float length =
        desktop::math::GetScalar(desktop::math::Magnitude(desktop::math::Sub(params.end, params.start)));

    desktop::Matrix T0 = desktop::math::LoadMatrix(
        right,
        tangent,
        params.normal,
        desktop::math::ExtendVector(params.start)
    );
    T0 = desktop::math::Transpose(T0);

    desktop::Matrix T1 = desktop::math::LoadMatrix(
        right,
        tangent,
        params.normal,
        desktop::math::ExtendVector(params.end)
    );
    T1 = desktop::math::Transpose(T1);

    const desktop::Vector v0 = { params.patternHeight / 2.0f, 0.0f, 0.0f, 1.0f };
    const desktop::Vector v1 = { -params.patternHeight / 2.0f, 0.0f, 0.0f, 1.0f };

    const float ds = 1 / (params.patternHeight * params.textureWidthHeightRatio);

    desktop::Vertex *vertex = writeVertex();
    vertex->Normal = params.normal;
    vertex->Pos = desktop::math::MatMult(T0, v0);
    vertex->TexCoord = desktop::Vector2(0.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = params.normal;
    vertex->Pos = desktop::math::MatMult(T0, v1);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = params.normal;
    vertex->Pos = desktop::math::MatMult(T1, v0);
    vertex->TexCoord = desktop::Vector2(ds * length + params.textureOffset, 1.0f);

    vertex = writeVertex();
    vertex->Normal = params.normal;
    vertex->Pos = desktop::math::MatMult(T1, v1);
    vertex->TexCoord = desktop::Vector2(ds * length + params.textureOffset, 0.0f);

    writeFace(0, 1, 2);
    writeFace(1, 3, 2);

    if (params.taperTail > 0) {
        const int steps = 10;
        const float step = params.taperTail / steps;
        for (int i = 0; i < steps; ++i) {
            const float scale = (steps - i - 1) / (float)steps;

            vertex = writeVertex();
            vertex->Normal = params.normal;
            vertex->Pos = desktop::math::MatMult(
                    T0,
                    desktop::math::LoadVector(
                        scale * params.patternHeight / 2.0f,
                        -step * (i + 1),
                        0.0f,
                        1.0f));
            vertex->TexCoord = desktop::Vector2(-ds * step * (i + 1) + params.textureOffset, 1.0f);

            vertex = writeVertex();
            vertex->Normal = params.normal;
            vertex->Pos = desktop::math::MatMult(
                    T0,
                    desktop::math::LoadVector(
                        -scale * params.patternHeight / 2.0f,
                        -step * (i + 1),
                        0.0f,
                        1.0f));
            vertex->TexCoord = desktop::Vector2(-ds * step * (i + 1) + params.textureOffset, 0.0f);

            vertex = writeVertex();
            vertex->Normal = params.normal;
            vertex->Pos = desktop::math::MatMult(
                    T1,
                    desktop::math::LoadVector(
                        scale * params.patternHeight / 2.0f,
                        step * (i + 1),
                        0.0f,
                        1.0f));
            vertex->TexCoord = desktop::Vector2(
                    ds * (length + step * (i + 1)) + params.textureOffset,
                    1.0f);

            vertex = writeVertex();
            vertex->Normal = params.normal;
            vertex->Pos = desktop::math::MatMult(
                    T1, desktop::math::LoadVector(
                        -scale * params.patternHeight / 2.0f,
                        step * (i + 1),
                        0.0f,
                        1.0f));
            vertex->TexCoord = desktop::Vector2(
                    ds * (length + step * (i + 1)) + params.textureOffset, 0.0f);

            writeFace((i + 1) * 4, (i + 1) * 4 + 1, i * 4);
            writeFace((i + 1) * 4 + 1, i * 4 + 1, i * 4);

            writeFace(i * 4 + 2, i * 4 + 3, (i + 1) * 4 + 2);
            writeFace(i * 4 + 3, (i + 1) * 4 + 3, (i + 1) * 4 + 2);
        }
    }

    return true;
}

bool GeometryGenerator::generateLine2d(
        const Line2dParameters &params)
{
    startSubshape();

    const float dx = params.x1 - params.x0;
    const float dy = params.y1 - params.y0;
    const float length = std::sqrt(dx * dx + dy * dy);

    // An unlit flame has coincident endpoints and contributes no triangles.
    if (length == 0.0f) return true;

    const float dir_x = dx / length;
    const float dir_y = dy / length;

    const float perp_x = -dir_y;
    const float perp_y = dir_x;

    const int vertexCount = 4;
    const int indexCount = 6;

    if (!checkCapacity(vertexCount, indexCount)) {
        return false;
    }

    desktop::Vertex *vertex;

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
            params.x0 + perp_x * params.lineWidth / 2,
            params.y0 + perp_y * params.lineWidth / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.x0 - perp_x * params.lineWidth / 2,
        params.y0 - perp_y * params.lineWidth / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.x1 + perp_x * params.lineWidth / 2,
        params.y1 + perp_y * params.lineWidth / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
            params.x1 - perp_x * params.lineWidth / 2,
            params.y1 - perp_y * params.lineWidth / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    writeFace(0, 1, 2);
    writeFace(1, 3, 2);

    return true;
}

bool GeometryGenerator::generateFrame(const FrameParameters &params) {
    State prevState = m_state;

    Line2dParameters lineParams;
    lineParams.lineWidth = params.lineWidth;

    lineParams.x0 = params.x - params.frameWidth / 2 - params.lineWidth / 2;
    lineParams.x1 = params.x + params.frameWidth / 2 + params.lineWidth / 2;

    lineParams.y0 = lineParams.y1 = params.y + params.frameHeight / 2;
    if (!generateLine2d(lineParams)) goto fail;

    lineParams.y0 = lineParams.y1 = params.y - params.frameHeight / 2;
    if (!generateLine2d(lineParams)) goto fail;

    lineParams.y0 = params.y - params.frameHeight / 2 - params.lineWidth / 2;
    lineParams.y1 = params.y + params.frameHeight / 2 + params.lineWidth / 2;

    lineParams.x0 = lineParams.x1 = params.x + params.frameWidth / 2;
    if (!generateLine2d(lineParams)) goto fail;

    lineParams.x0 = lineParams.x1 = params.x - params.frameWidth / 2;
    if (!generateLine2d(lineParams)) goto fail;

    return true;

fail:
    m_state = prevState;
    return false;
}

bool GeometryGenerator::generateGrid(const GridParameters &params) {
    const State prevState = m_state;

    Line2dParameters lineParams;
    lineParams.lineWidth = params.lineWidth;

    lineParams.x0 = params.x - params.width / 2;
    lineParams.x1 = params.x + params.width / 2;

    for (float offset = 0.0f; offset <= params.height / 2.0f; offset += params.div_y) {
        lineParams.y0 = lineParams.y1 = params.y + offset;
        if (!generateLine2d(lineParams)) goto fail;

        lineParams.y0 = lineParams.y1 = params.y - offset;
        if (!generateLine2d(lineParams)) goto fail;
    }

    lineParams.y0 = params.y - params.height / 2;
    lineParams.y1 = params.y + params.height / 2;

    for (float offset = 0.0f; offset <= params.width / 2.0f; offset += params.div_x) {
        lineParams.x0 = lineParams.x1 = params.x + offset;
        if (!generateLine2d(lineParams)) goto fail;

        lineParams.x0 = lineParams.x1 = params.x - offset;
        if (!generateLine2d(lineParams)) goto fail;
    }

    return true;

fail:
    m_state = prevState;
    return false;
}

bool GeometryGenerator::generateRing2d(const Ring2dParameters &params) {
    // edge_length = (sin(theta) * radius) * 2
    // theta = arcsin(edge_length / (2 * radius))

    startSubshape();

    const float angle = ::asinf(params.maxEdgeLength / (2 * params.outerRadius));
    const float steps = (params.endAngle - params.startAngle) / angle;

    int segmentCount = (int)::ceilf(steps);
    segmentCount = (segmentCount < 3)
        ? 3
        : segmentCount;

    const int vertexCount = (segmentCount + 1) * 2;
    const int faceCount = segmentCount * 2;
    const int indexCount = faceCount * 3;

    if (!checkCapacity(vertexCount, indexCount)) {
        return false;
    }

    const float angleStep = (params.endAngle - params.startAngle) / segmentCount;

    float arrowStart = 0.0f;
    float arrowEnd = 0.0f;
    if (params.drawArrow) {
        if (params.arrowOnEnd) {
            arrowStart = params.endAngle - params.arrowLength;
            arrowEnd = params.endAngle;
        }
        else {
            arrowStart = params.startAngle + params.arrowLength;
            arrowEnd = params.startAngle;
        }
    }

    const float midRadius = (params.outerRadius + params.innerRadius) / 2;
    const float fullWidth = params.outerRadius - params.innerRadius;
    for (int i = 0; i <= segmentCount; ++i) {
        float angle0 = angleStep * i + params.startAngle;
        const float x0 = ::cosf(angle0);
        const float y0 = ::sinf(angle0);

        const float s = (params.drawArrow)
            ? (angle0 - arrowStart) / (arrowEnd - arrowStart)
            : 0.0f;

        const float width = fullWidth * (1 - std::fminf(1.0, std::fmaxf(s, 0.0)));
        const float innerRadius = midRadius - width / 2;
        const float outerRadius = midRadius + width / 2;

        const desktop::Vector outerPos =
            desktop::math::LoadVector(
                    x0 * outerRadius + params.center_x,
                    y0 * outerRadius + params.center_y, 0.0f, 1.0f);
        const desktop::Vector innerPos =
            desktop::math::LoadVector(
                    x0 * innerRadius + params.center_x,
                    y0 * innerRadius + params.center_y, 0.0f, 1.0f);

        desktop::Vertex *outerVertex = writeVertex();
        outerVertex->Normal = desktop::math::Constants::ZAxis;
        outerVertex->Pos = outerPos;
        outerVertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

        desktop::Vertex *innerVertex = writeVertex();
        innerVertex->Normal = desktop::math::Constants::ZAxis;
        innerVertex->Pos = innerPos;
        innerVertex->TexCoord = desktop::Vector2(0.0f, 0.0f);
    }

#define OUTER(i) (((i)) * 2)
#define INNER(i) (((i)) * 2 + 1)

    for (int i = 0; i < segmentCount; ++i) {
        writeFace(INNER(i), OUTER(i + 1), INNER(i + 1));
        writeFace(INNER(i), OUTER(i), OUTER(i + 1));
    }

    return true;
}

bool GeometryGenerator::generateCircle2d(const Circle2dParameters &params) {
    // edge_length = (sin(theta) * radius) * 2
    // theta = arcsin(edge_length / (2 * radius))
    // theta2 = PI - theta

    startSubshape();

    float angle = ::asinf(params.maxEdgeLength / (2 * params.radius)) * 2;
    angle = std::fminf(angle, desktop::math::Constants::PI - params.smallestAngle);

    const float steps = desktop::math::Constants::TWO_PI / angle;

    int segmentCount = (int)::ceilf(steps);
    segmentCount = (segmentCount < 3)
        ? 3
        : segmentCount;

    const int vertexCount = 1 + segmentCount;
    const int faceCount = segmentCount;
    const int indexCount = faceCount * 3;

    if (!checkCapacity(vertexCount, indexCount)) {
        return false;
    }

    // Generate center vertex
    desktop::Vertex *centerVertex = writeVertex();
    centerVertex->Normal = desktop::math::Constants::ZAxis;
    centerVertex->Pos = desktop::math::LoadVector(params.center_x, params.center_y);
    centerVertex->TexCoord = desktop::Vector2(0.5f, 0.5f);

    const float angleStep = desktop::math::Constants::TWO_PI / segmentCount;

    for (int i = 0; i < segmentCount; ++i) {
        const float angle0 = angleStep * i;
        const float x = ::cosf(angle0);
        const float y = ::sinf(angle0);

        const float pos_x = params.center_x + x * params.radius;
        const float pos_y = params.center_y + y * params.radius;

        desktop::Vertex *newVertex = writeVertex();
        newVertex->Normal.Set(0, 0, 1, 0);
        newVertex->Pos.Set(pos_x, pos_y, 0.0f, 1.0f);
        newVertex->TexCoord.x = 0.5f * x + 0.5f;
        newVertex->TexCoord.y = 0.5f * y + 0.5f;
    }

    for (int i = 0; i < segmentCount; ++i) {
        writeFace(0, i + 1, 1 + ((i + 1) % segmentCount));
    }

    return true;
}

bool GeometryGenerator::generateCam(const Cam2dParameters &params) {
    // edge_length = (sin(theta) * radius) * 2
    // theta = arcsin(edge_length / (2 * radius))
    // theta2 = PI - theta

    startSubshape();

    float angle = ::asinf(params.maxEdgeLength / (2 * params.baseRadius)) * 2;
    angle = std::fminf(angle, desktop::math::Constants::PI - params.smallestAngle);

    const float steps = desktop::math::Constants::TWO_PI / angle;

    int segmentCount = (int)::ceilf(steps);
    segmentCount = (segmentCount < 3)
        ? 3
        : segmentCount;

    const int vertexCount = 1 + segmentCount;
    const int faceCount = segmentCount;
    const int indexCount = faceCount * 3;

    if (!checkCapacity(vertexCount, indexCount)) {
        return false;
    }

    // Generate center vertex
    desktop::Vertex *centerVertex = writeVertex();
    centerVertex->Normal = desktop::math::Constants::ZAxis;
    centerVertex->Pos = desktop::math::LoadVector(params.center_x, params.center_y);
    centerVertex->TexCoord = desktop::Vector2(0.5f, 0.5f);

    const float angleStep = desktop::math::Constants::TWO_PI / segmentCount;
    const float baseRollerPosition = params.baseRadius + params.rollerRadius;

    float lastRoller_x = 0;
    float lastRoller_y = 0;

    float *positions_x = new float[segmentCount];
    float *positions_y = new float[segmentCount];

    for (int i = 0; i < segmentCount; ++i) {
        const float angle0 = angleStep * i;
        const float x = ::cosf(angle0 + desktop::math::Constants::PI / 2);
        const float y = ::sinf(angle0 + desktop::math::Constants::PI / 2);

        const float lift = (params.lift == nullptr)
            ? 0.0f
            : (float)params.lift->sampleTriangle((double)angle0 - desktop::math::Constants::TWO_PI / 2);
        const float rollerPosition = baseRollerPosition + lift;

        const float rollerPos_x = params.center_x + x * rollerPosition;
        const float rollerPos_y = params.center_y + y * rollerPosition;

        float dx = -1, dy = 0;
        if (i != 0) {
            dx = rollerPos_x - lastRoller_x;
            dy = rollerPos_y - lastRoller_y;

            const float mag = std::sqrt(dx * dx + dy * dy);
            dx /= mag;
            dy /= mag;
        }

        lastRoller_x = rollerPos_x;
        lastRoller_y = rollerPos_y;

        const float t_dx = -dy;
        const float t_dy = dx;

        const float tangentPos_x = rollerPos_x + t_dx * params.rollerRadius;
        const float tangentPos_y = rollerPos_y + t_dy * params.rollerRadius;

        positions_x[i] = tangentPos_x;
        positions_y[i] = tangentPos_y;
    }

    for (int i = 0; i < segmentCount; ++i) {
        const int prev_i = (i - 1 + segmentCount) % segmentCount;
        const int next_i = (i + 1) % segmentCount;

        const float prev_x = positions_x[prev_i], prev_y = positions_y[prev_i];
        const float next_x = positions_x[next_i], next_y = positions_y[next_i];

        desktop::Vertex *newVertex = writeVertex();
        newVertex->Normal.Set(0, 0, 1, 0);
        newVertex->Pos.Set((prev_x + next_x) / 2, (prev_y + next_y) / 2, 0.0f, 1.0f);
        newVertex->TexCoord.x = 0.0f;
        newVertex->TexCoord.y = 0.5f;
    }

    for (int i = 0; i < segmentCount; ++i) {
        writeFace(0, i + 1, 1 + ((i + 1) % segmentCount));
    }

    delete[] positions_x;
    delete[] positions_y;

    return true;
}

bool GeometryGenerator::generateRhombus(const Rhombus2dParameters &params) {
    startSubshape();

    if (!checkCapacity(4, 6)) {
        return false;
    }

    desktop::Vertex *vertex;

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
            params.center_x + params.shear + params.width / 2,
            params.center_y + params.height / 2);
    vertex->TexCoord = desktop::Vector2(1.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
            params.center_x + params.shear - params.width / 2,
            params.center_y + params.height / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.center_x - params.shear + params.width / 2,
        params.center_y - params.height / 2);
    vertex->TexCoord = desktop::Vector2(1.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
            params.center_x - params.shear - params.width / 2,
            params.center_y - params.height / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    writeFace(0, 1, 2);
    writeFace(1, 3, 2);

    return true;
}

bool GeometryGenerator::generateTrapezoid2d(const Trapezoid2dParameters &params) {
    startSubshape();

    if (!checkCapacity(3, 3)) {
        return false;
    }

    desktop::Vertex *vertex;

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.center_x - params.base / 2, params.center_y - params.height / 2);
    vertex->TexCoord = desktop::Vector2(1.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.center_x + params.base / 2, params.center_y - params.height / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.center_x - params.top / 2, params.center_y + params.height / 2);
    vertex->TexCoord = desktop::Vector2(1.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.center_x + params.top / 2, params.center_y + params.height / 2);
    vertex->TexCoord = desktop::Vector2(1.0f, 0.0f);

    writeFace(0, 1, 2);
    writeFace(1, 3, 2);

    return true;
}

bool GeometryGenerator::generateIsoscelesTriangle(
        float x,
        float y,
        float width,
        float height)
{
    startSubshape();

    if (!checkCapacity(3, 3)) {
        return false;
    }

    desktop::Vertex *vertex;

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(x - width / 2, y);
    vertex->TexCoord = desktop::Vector2(1.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(x + width / 2, y);
    vertex->TexCoord = desktop::Vector2(0.0f, 1.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(x, y + height);
    vertex->TexCoord = desktop::Vector2(1.0f, 0.0f);

    writeFace(0, 1, 2);

    return true;
}

bool GeometryGenerator::startPath(PathParameters &params) {
    startSubshape();

    const int n = params.n0 + params.n1;
    if (n < 2) return true;

    float dx=0, dy=0, length=0;
    for (int next=1; next<n; ++next) {
        const Point *p2 = (next >= params.n0) ? params.p1 : params.p0;
        const int index = (next >= params.n0) ? next-params.n0 : next;
        dx=p2[index].x-params.p0[0].x; dy=p2[index].y-params.p0[0].y;
        length=std::sqrt(dx*dx+dy*dy);
        if (length!=0) break;
    }
    const float dir_x = length==0 ? 0 : dx/length;
    const float dir_y = length==0 ? 0 : dy/length;

    const float perp_x = -dir_y;
    const float perp_y = dir_x;

    if (!checkCapacity(n * 2, (n - 1) * 6)) {
        return false;
    }

    desktop::Vertex *vertex;

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.p0[0].x + perp_x * params.width / 2,
        params.p0[0].y + perp_y * params.width / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        params.p0[0].x - perp_x * params.width / 2,
        params.p0[0].y - perp_y * params.width / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    params.v0 = 0;
    params.v1 = 1;
    params.pdir_x = dir_x;
    params.pdir_y = dir_y;
    params.perp_x = perp_x;
    params.perp_y = perp_y;

    return true;
}

bool GeometryGenerator::generatePathSegment(PathParameters &params, bool detached) {
    const int n = params.n0 + params.n1;

    if (params.i > n - 1) return true;

    Point *p0 = (params.i >= params.n0) ? params.p1 : params.p0;
    const int i0 = (params.i >= params.n0) ? (params.i - params.n0) : params.i;

    if (params.i == n - 1) {
        desktop::Vertex *vertex;
        vertex = writeVertex();
        vertex->Normal = desktop::math::Constants::ZAxis;
        vertex->Pos = desktop::math::LoadVector(
            p0[i0].x + params.perp_x * params.width / 2,
            p0[i0].y + params.perp_y * params.width / 2);
        vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

        vertex = writeVertex();
        vertex->Normal = desktop::math::Constants::ZAxis;
        vertex->Pos = desktop::math::LoadVector(
            p0[i0].x - params.perp_x * params.width / 2,
            p0[i0].y - params.perp_y * params.width / 2);
        vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

        if (!detached) {
            writeFace(params.v0, params.v1, params.v1 + 1);
            writeFace(params.v1, params.v1 + 2, params.v1 + 1);
        }

        return true;
    }

    Point *p1 = (params.i + 1 >= params.n0) ? params.p1 : params.p0;
    const int i1 = (params.i + 1 >= params.n0) ? (params.i + 1 - params.n0) : params.i + 1;

    const float dx1 = p1[i1].x - p0[i0].x;
    const float dy1 = p1[i1].y - p0[i0].y;
    const float length = std::sqrt(dx1 * dx1 + dy1 * dy1);

    const float dir_x = length==0 ? params.pdir_x : dx1/length;
    const float dir_y = length==0 ? params.pdir_y : dy1/length;

    float perp_x = -dir_y - params.pdir_y;
    float perp_y = dir_x + params.pdir_x;
    float perp_l = std::sqrt(perp_x * perp_x + perp_y * perp_y);
    if (perp_l == 0) {
        perp_x = -dir_y;
        perp_y = dir_x;
    }
    else {
        perp_x /= perp_l;
        perp_y /= perp_l;
    }

    desktop::Vertex *vertex;

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        p0[i0].x + perp_x * params.width / 2,
        p0[i0].y + perp_y * params.width / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    vertex = writeVertex();
    vertex->Normal = desktop::math::Constants::ZAxis;
    vertex->Pos = desktop::math::LoadVector(
        p0[i0].x - perp_x * params.width / 2,
        p0[i0].y - perp_y * params.width / 2);
    vertex->TexCoord = desktop::Vector2(0.0f, 0.0f);

    if (!detached) {
        writeFace(params.v0, params.v1, params.v1 + 1);
        writeFace(params.v1, params.v1 + 2, params.v1 + 1);
    }

    params.v0 = params.v1 + 1;
    params.v1 = params.v1 + 2;
    params.pdir_x = dir_x;
    params.pdir_y = dir_y;
    params.perp_x = perp_x;
    params.perp_y = perp_y;

    return true;
}

desktop::Vertex *GeometryGenerator::writeVertex() {
    return &m_vertexData[m_state.vertexPointer++];
}

void GeometryGenerator::startShape() {
    m_state.currentShape.BaseIndex = m_state.indexPointer;
    m_state.currentShape.BaseVertex = m_state.vertexPointer;
    m_state.currentShape.FaceCount = 0;
    m_state.currentShape.VertexData = &m_vertexData[m_state.vertexPointer];
}

void GeometryGenerator::endShape(GeometryIndices *indices) {
    *indices = m_state.currentShape;
}

void GeometryGenerator::startSubshape() {
    m_state.subshapeVertexPointer = m_state.vertexPointer - m_state.currentShape.BaseVertex;
}

void GeometryGenerator::writeFace(unsigned short i0, unsigned short i1, unsigned short i2) {
    m_indexData[m_state.indexPointer + 0] = i0 + m_state.subshapeVertexPointer;
    m_indexData[m_state.indexPointer + 1] = i1 + m_state.subshapeVertexPointer;
    m_indexData[m_state.indexPointer + 2] = i2 + m_state.subshapeVertexPointer;

    ++m_state.currentShape.FaceCount;

    m_state.indexPointer += 3;
}

bool GeometryGenerator::checkCapacity(int vertexCount, int indexCount) {
    return
        (vertexCount + m_state.vertexPointer) <= m_vertexBufferSize &&
        (indexCount + m_state.indexPointer) <= m_indexBufferSize;
}

desktop::Vector GeometryGenerator::findOrthogonal(const desktop::Vector &v) {
    desktop::Vector base = desktop::math::Constants::XAxis;
    const float s = desktop::math::GetScalar(desktop::math::Dot(v, base));
    if (abs(s) > 0.99f) {
        base = desktop::math::Constants::YAxis;
    }

    return desktop::math::Normalize(desktop::math::Cross(v, base));
}
