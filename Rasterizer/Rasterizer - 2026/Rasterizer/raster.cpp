#include <iostream>
#define _USE_MATH_DEFINES
#include <cmath>

#include "GamesEngineeringBase.h" // Include the GamesEngineeringBase header
#include <algorithm>
#include <chrono>


//SIMD
#include <immintrin.h>
//Multi-Thread
#include <thread>


#include <cmath>
#include "matrix.h"
#include "colour.h"
#include "mesh.h"
#include "zbuffer.h"
#include "renderer.h"
#include "RNG.h"
#include "light.h"
#include "triangle.h"

class ThreadSys {
public:
    //pointers (to read thread data)
    Renderer* currentRenderer = nullptr;
    matrix* currentCamera = nullptr;
    Light* currentLight = nullptr;
    std::vector<Mesh*>* currentScene = nullptr;

    //thread m
    std::vector<std::thread> threadPool;
    //make threads exit infinite loop
    std::atomic<bool> stop{ false };

    //used to see when thread tasks are complete
    std::atomic<int> threadTaskCount{ 0 };
    //start loop
    std::atomic<int> start{ 0 };

    //see what geom work next (atomic because two threads can catch)
    std::atomic<size_t> geomCount{ 0 };
    std::atomic<size_t> pTileCount{ 0 };

    //pipleline state - swap between geom/raster
    enum class PipelineState { geomState, rasterizeState };
    std::atomic<PipelineState> currentState{ PipelineState::geomState };

    //tile (screen)
    int tileSize = 32;
    int gridW = 0;
    int gridH = 0;

    //canvas sizing
    float canvasW = 0.f;
    float canvasH = 0.f;

    //triangle struct (geom)
    struct mainTri {
        Vertex v[3];
        float ka;
        float kd;
        int minX, maxX, minY, maxY;
    };

    //buffer
    //main triangle control
    std::vector<mainTri> triControl;

    std::vector<std::vector<mainTri>> threadGeomCache;
    std::vector<std::vector<int>> tileTList;

    //main run call
    void run(Renderer& r, std::vector<Mesh*>& meshes, matrix& cam, Light& light) {
        //pointers
        currentRenderer = &r;
        currentCamera = &cam;
        currentLight = &light;
        currentScene = &meshes;

        //call initialising threads first
        initThreads();

        //geom
        //set state to geom first
        currentState.store(PipelineState::geomState);
        //reset to 0
        geomCount.store(0);
        threadTaskCount.store(0);
        //start threads 
        start.fetch_add(1);

        syncThreads();

        //merge thread buffer to one
        mergeGeom();
        //sort grid triangles
        sortTriLists();

        //rasterize
        //drawing happens - set rasterize
        currentState.store(PipelineState::rasterizeState);
        //reset to 0
        pTileCount.store(0);
        threadTaskCount.store(0);
        //start threads
        start.fetch_add(1);

        syncThreads();
    }

    void initThreads() {
        //if nothing in pool return
        if (!threadPool.empty()) return;

        //thread count (hardware_concurreny for max threads)
        int cores = std::thread::hardware_concurrency();

        //resize so each thread owns
        threadPool.resize(cores);
        threadGeomCache.resize(cores);


        for (int i = 0; i < cores; ++i) {
            threadPool[i] = std::thread([this, i]() {
                //startthread signal
                int startThread = 0;

                while (true) {
                    //wait until signal changed
                    while (start.load() == startThread) {
                        if (stop.load()) return;
                        std::this_thread::yield();
                    }

                    //if signal changed then update
                    startThread = start.load();

                    //check the state and run (if state is geom then run the geometry)
                    if (currentState.load() == PipelineState::geomState)
                        //vertices
                        executegeomState(i);
                    else
                        //rasterize
                        executerasterizeState(i);

                    //add to finish
                    threadTaskCount.fetch_add(1);
                }
                });
        }
    }

    void syncThreads() {
        while (threadTaskCount.load() < threadPool.size())
            std::this_thread::yield();
    }

    void executegeomState(int threadID) {
        //pointers
        Renderer& r = *currentRenderer;
        matrix cam = *currentCamera;

        //canvas w and h
        canvasW = (float)r.canvas.getWidth();
        canvasH = (float)r.canvas.getHeight();

        auto& scene = *currentScene;
        size_t totalMesh = scene.size();

        while (true) {
            //get next mesh
            size_t idx = geomCount.fetch_add(1);

            //if done (reach all mesh)
            if (idx >= totalMesh) break;


            Mesh* mesh = scene[idx];
            matrix mvp = r.perspective * cam * mesh->world;

            //triangle loop - check every tri in mesh
            for (auto& face : mesh->triangles) {
                mainTri tri;
                tri.ka = mesh->ka;
                tri.kd = mesh->kd;
                bool skip = false;

                for (int k = 0; k < 3; ++k) {
                    unsigned int vIdx = face.v[k];

                    //transform
                    tri.v[k].p = mvp * mesh->vertices[vIdx].p;
                    tri.v[k].p.divideW();

                    //zclip
                    if (fabs(tri.v[k].p[2]) > 1.0f) {
                        skip = true;
                        break;
                    }

                    //viewport
                    tri.v[k].p[0] = (tri.v[k].p[0] + 1.f) * 0.5f * canvasW;
                    tri.v[k].p[1] = (tri.v[k].p[1] + 1.f) * 0.5f * canvasH;
                    tri.v[k].p[1] = canvasH - tri.v[k].p[1];

                    //colour/norm
                    tri.v[k].normal = mesh->world * mesh->vertices[vIdx].normal;
                    tri.v[k].normal.normalise();
                    tri.v[k].rgb = mesh->vertices[vIdx].rgb;
                }

                if (skip) continue;
                //calc where triangle should be
                setBound(tri);
                //write thread cache
                threadGeomCache[threadID].push_back(tri);
            }
        }
    }

    void setBound(mainTri& tri) {
        tri.minX = std::min({ tri.v[0].p[0], tri.v[1].p[0], tri.v[2].p[0] });
        tri.maxX = std::max({ tri.v[0].p[0], tri.v[1].p[0], tri.v[2].p[0] });
        tri.minY = std::min({ tri.v[0].p[1], tri.v[1].p[1], tri.v[2].p[1] });
        tri.maxY = std::max({ tri.v[0].p[1], tri.v[1].p[1], tri.v[2].p[1] });

        tri.minX = std::max(0, tri.minX);
        tri.minY = std::max(0, tri.minY);
        tri.maxX = std::min((int)canvasW - 1, tri.maxX);
        tri.maxY = std::min((int)canvasH - 1, tri.maxY);
    }

    void mergeGeom() {
        triControl.clear();
        for (auto& buffer : threadGeomCache) {
            triControl.insert(triControl.end(), buffer.begin(), buffer.end());
            buffer.clear();
        }
    }

    void sortTriLists() {
        int w = currentRenderer->canvas.getWidth();
        int h = currentRenderer->canvas.getHeight();

        //resize grid
        gridW = (w + tileSize - 1) / tileSize;
        gridH = (h + tileSize - 1) / tileSize;

        tileTList.clear();
        tileTList.resize(gridW * gridH);

        //triangle loop - run through every tri
        for (int i = 0; i < triControl.size(); ++i) {
            auto& t = triControl[i];

            //convert triangle to grid
            int startX = t.minX / tileSize;
            int endX = t.maxX / tileSize;
            int startY = t.minY / tileSize;
            int endY = t.maxY / tileSize;

            for (int y = startY; y <= endY; ++y) {
                for (int x = startX; x <= endX; ++x) {
                    tileTList[y * gridW + x].push_back(i);
                }
            }
        }
    }

    void executerasterizeState(int threadID) {
        //pointers
        Renderer& r = *currentRenderer;
        Light light = *currentLight;

        while (true) {
            //grab next tile
            size_t tileID = pTileCount.fetch_add(1);
            //if out of bound then breakl (none left)
            if (tileID >= tileTList.size()) break;

            //calculate bounds
            int gridX = tileID % gridW;
            int gridY = tileID / gridW;

            int xStart = gridX * tileSize;
            int yStart = gridY * tileSize;

            //loop through tri in this grid
            for (int triIdx : tileTList[tileID]) {
                //tri been processed
                auto& pTri = triControl[triIdx];
                //redo triangle
                triangle tri(pTri.v[0], pTri.v[1], pTri.v[2]);
                //draw 
                tri.drawClipped(r, light, pTri.ka, pTri.kd, xStart, yStart, xStart + tileSize, yStart + tileSize);
            }
        }
    }


    ~ThreadSys() {
        stop.store(true);
        start.fetch_add(1);
        for (auto& t : threadPool) t.join();
    }
};

struct MeshSoA {
    std::vector<float> x, y, z;
    size_t size = 0;

    void init(const std::vector<Vertex>& verts) {
        size = verts.size();
        x.resize(size); y.resize(size); z.resize(size);
        for (size_t i = 0; i < size; i++) {
            x[i] = verts[i].p[0];
            y[i] = verts[i].p[1];
            z[i] = verts[i].p[2];
        }
    }
};

void simdSet(const MeshSoA& in, MeshSoA& out, const matrix& m, float w, float h) {
    //Load
    const float* mat = reinterpret_cast<const float*>(&m);
    __m128 m00 = _mm_set1_ps(mat[0]);  __m128 m01 = _mm_set1_ps(mat[1]);  __m128 m02 = _mm_set1_ps(mat[2]);  __m128 m03 = _mm_set1_ps(mat[3]);
    __m128 m10 = _mm_set1_ps(mat[4]);  __m128 m11 = _mm_set1_ps(mat[5]);  __m128 m12 = _mm_set1_ps(mat[6]);  __m128 m13 = _mm_set1_ps(mat[7]);
    __m128 m20 = _mm_set1_ps(mat[8]);  __m128 m21 = _mm_set1_ps(mat[9]);  __m128 m22 = _mm_set1_ps(mat[10]); __m128 m23 = _mm_set1_ps(mat[11]);
    __m128 m30 = _mm_set1_ps(mat[12]); __m128 m31 = _mm_set1_ps(mat[13]); __m128 m32 = _mm_set1_ps(mat[14]); __m128 m33 = _mm_set1_ps(mat[15]);

    __m128 vOne = _mm_set1_ps(1.0f);
    __m128 vHalf = _mm_set1_ps(0.5f);
    __m128 vW = _mm_set1_ps(w);
    __m128 vH = _mm_set1_ps(h);

    //Process 4 vert
    for (size_t i = 0; i <= in.size - 4; i += 4) {
        __m128 x = _mm_loadu_ps(&in.x[i]);
        __m128 y = _mm_loadu_ps(&in.y[i]);
        __m128 z = _mm_loadu_ps(&in.z[i]);
        __m128 w_in = vOne;

        //Multiply
        __m128 rx = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m00), _mm_mul_ps(y, m10)), _mm_add_ps(_mm_mul_ps(z, m20), _mm_mul_ps(w_in, m30)));
        __m128 ry = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m01), _mm_mul_ps(y, m11)), _mm_add_ps(_mm_mul_ps(z, m21), _mm_mul_ps(w_in, m31)));
        __m128 rz = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m02), _mm_mul_ps(y, m12)), _mm_add_ps(_mm_mul_ps(z, m22), _mm_mul_ps(w_in, m32)));
        __m128 rw = _mm_add_ps(_mm_add_ps(_mm_mul_ps(x, m03), _mm_mul_ps(y, m13)), _mm_add_ps(_mm_mul_ps(z, m23), _mm_mul_ps(w_in, m33)));

        //Div
        rx = _mm_div_ps(rx, rw);
        ry = _mm_div_ps(ry, rw);
        rz = _mm_div_ps(rz, rw);

        //Map
        rx = _mm_mul_ps(_mm_mul_ps(_mm_add_ps(rx, vOne), vHalf), vW);
        ry = _mm_sub_ps(vH, _mm_mul_ps(_mm_mul_ps(_mm_add_ps(ry, vOne), vHalf), vH));

        _mm_storeu_ps(&out.x[i], rx);
        _mm_storeu_ps(&out.y[i], ry);
        _mm_storeu_ps(&out.z[i], rz);
    }
}

// Main rendering function that processes a mesh, transforms its vertices, applies lighting, and draws triangles on the canvas.
// Input Variables:
// - renderer: The Renderer object used for drawing.
// - mesh: Pointer to the Mesh object containing vertices and triangles to render.
// - camera: Matrix representing the camera's transformation.
// - L: Light object representing the lighting parameters.
void render(Renderer& renderer, Mesh* mesh, matrix& camera, Light& L) {
    //FRUSTRUM CULLING
    //get mid vec4 (x = 0, y = 0, z = 0)
    vec4 mid = vec4(0, 0, 0, 1);
    //transform
    mid = renderer.perspective * camera * mesh->world * mid;
    if (fabs(mid[3]) > 0.001f) {
        mid.divideW();
    }
    else {
        return; //bh cam skip
    }

    float margin = 0.5f; //(if partly on)
    //limits/bounds get
    float boundX = 1.0f + (mesh->boundSphereRad * margin);
    float boundY = 1.0f + (mesh->boundSphereRad * margin);
    //skips
    if (mid[2] < 0.0f or mid[2] > 1.0f) return;

    //check w
    if (mid[0] < -boundX or mid[0] > boundX) return;

    //check h
    if (mid[1] < -boundY or mid[1] > boundY) return;

    //SIMD
    MeshSoA soaInput;
    MeshSoA soaOutput;


    if (soaInput.size != mesh->vertices.size()) {
        soaInput.init(mesh->vertices);

        //resize match input
        soaOutput.x.resize(soaInput.size);
        soaOutput.y.resize(soaInput.size);
        soaOutput.z.resize(soaInput.size);
        soaOutput.size = soaInput.size;
    }
    //SIMD

    // Combine perspective, camera, and world transformations for the mesh
    matrix p = renderer.perspective * camera * mesh->world;

    //SIMD
    //call transform
    simdSet(soaInput, soaOutput, p, (float)renderer.canvas.getWidth(), (float)renderer.canvas.getHeight());
    //SIMD

    // Iterate through all triangles in the mesh
    for (triIndices& ind : mesh->triangles) {
        Vertex t[3]; // Temporary array to store transformed triangle vertices

        //{CHANGE}
        //hw OPTIMISED - can be used to get height rather than recalling multiple times
        float width = static_cast<float>(renderer.canvas.getWidth());
        float height = static_cast<float>(renderer.canvas.getHeight());



        // Transform each vertex of the triangle

        //SIMD
        for (unsigned int i = 0; i < 3; i++) {
            unsigned int vIdx = ind.v[i]; // Get the vertex index

            //retrieve simd out
            t[i].p[0] = soaOutput.x[vIdx];
            t[i].p[1] = soaOutput.y[vIdx];
            t[i].p[2] = soaOutput.z[vIdx];
            //w div done simd
            t[i].p[3] = 1.0f;

            t[i].normal = mesh->world * mesh->vertices[vIdx].normal;
            t[i].normal.normalise();
            t[i].rgb = mesh->vertices[vIdx].rgb;
        }



        //SIMD

        //Clip triangles with Z-values outside [-1, 1]
        if (fabs(t[0].p[2]) > 1.0f || fabs(t[1].p[2]) > 1.0f || fabs(t[2].p[2]) > 1.0f) continue;

        // Create a triangle object and render it
       // triangle tri(t[0], t[1], t[2]);
       // tri.draw(renderer, L, mesh->ka, mesh->kd);




        //BACKFACE CULLING(only render what we can actually see / facing cam)
        vec4 e1 = t[1].p - t[0].p;
        vec4 e2 = t[2].p - t[0].p;
        vec4 normal = vec4::cross(e1, e2);

        //If facing draw (if not skip)
        if (normal[2] > 0.0f) {
            triangle tri(t[0], t[1], t[2]);
            tri.draw(renderer, L, mesh->ka, mesh->kd);
        }


    }


}

// Test scene function to demonstrate rendering with user-controlled transformations
// No input variables
void sceneTest() {
    Renderer renderer;
    // create light source {direction, diffuse intensity, ambient intensity}
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };
    // camera is just a matrix
    matrix camera = matrix::makeIdentity(); // Initialize the camera with identity matrix

    bool running = true; // Main loop control variable

    std::vector<Mesh*> scene; // Vector to store scene objects

    // Create a sphere and a rectangle mesh
    Mesh mesh = Mesh::makeSphere(1.0f, 10, 20);
    //Mesh mesh2 = Mesh::makeRectangle(-2, -1, 2, 1);

    // add meshes to scene
    scene.push_back(&mesh);
    // scene.push_back(&mesh2); 

    float x = 0.0f, y = 0.0f, z = -4.0f; // Initial translation parameters
    mesh.world = matrix::makeTranslation(x, y, z);
    //mesh2.world = matrix::makeTranslation(x, y, z) * matrix::makeRotateX(0.01f);

    // Main rendering loop
    while (running) {
        renderer.canvas.checkInput(); // Handle user input
        renderer.clear(); // Clear the canvas for the next frame

        // Apply transformations to the meshes
     //   mesh2.world = matrix::makeTranslation(x, y, z) * matrix::makeRotateX(0.01f);
        mesh.world = matrix::makeTranslation(x, y, z);

        // Handle user inputs for transformations
        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;
        if (renderer.canvas.keyPressed('A')) x += -0.1f;
        if (renderer.canvas.keyPressed('D')) x += 0.1f;
        if (renderer.canvas.keyPressed('W')) y += 0.1f;
        if (renderer.canvas.keyPressed('S')) y += -0.1f;
        if (renderer.canvas.keyPressed('Q')) z += 0.1f;
        if (renderer.canvas.keyPressed('E')) z += -0.1f;

        // Render each object in the scene
        //for (auto& m : scene)
            //ParallelRasterPipeline::execute(renderer, m, camera, L);

        renderer.present(); // Display the rendered frame
    }
}

// Utility function to generate a random rotation matrix
// No input variables
matrix makeRandomRotation() {
    RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();
    unsigned int r = rng.getRandomInt(0, 3);

    switch (r) {
    case 0: return matrix::makeRotateX(rng.getRandomFloat(0.f, 2.0f * M_PI));
    case 1: return matrix::makeRotateY(rng.getRandomFloat(0.f, 2.0f * M_PI));
    case 2: return matrix::makeRotateZ(rng.getRandomFloat(0.f, 2.0f * M_PI));
    default: return matrix::makeIdentity();
    }
}

// Function to render a scene with multiple objects and dynamic transformations
// No input variables
void scene1() {
    ThreadSys pipeline;
    std::vector<Mesh*> scene;
    Renderer renderer;
    matrix camera;
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    bool running = true;

    // Create a scene of 40 cubes with random rotations
    for (unsigned int i = 0; i < 20; i++) {
        Mesh* m = new Mesh();
        *m = Mesh::makeCube(1.f);
        m->world = matrix::makeTranslation(-2.0f, 0.0f, (-3 * static_cast<float>(i))) * makeRandomRotation();
        scene.push_back(m);
        m = new Mesh();
        *m = Mesh::makeCube(1.f);
        m->world = matrix::makeTranslation(2.0f, 0.0f, (-3 * static_cast<float>(i))) * makeRandomRotation();
        scene.push_back(m);
    }

    float zoffset = 8.0f; // Initial camera Z-offset
    float step = -0.1f;  // Step size for camera movement

    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> end;
    int cycle = 0;

    // Main rendering loop
    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        camera = matrix::makeTranslation(0, 0, -zoffset); // Update camera position

        // Rotate the first two cubes in the scene
        scene[0]->world = scene[0]->world * matrix::makeRotateXYZ(0.1f, 0.1f, 0.0f);
        scene[1]->world = scene[1]->world * matrix::makeRotateXYZ(0.0f, 0.1f, 0.2f);

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;

        zoffset += step;
        if (zoffset < -60.f || zoffset > 8.f) {
            step *= -1.f;
            if (++cycle % 2 == 0) {
                end = std::chrono::high_resolution_clock::now();
                std::cout << cycle / 2 << " :" << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";
                start = std::chrono::high_resolution_clock::now();
            }
        }

        pipeline.run(renderer, scene, camera, L);
        renderer.present();
    }

    for (auto& m : scene)
        delete m;
}

// Scene with a grid of cubes and a moving sphere
// No input variables
void scene2() {
    ThreadSys pipeline;
    Renderer renderer;
    matrix camera = matrix::makeIdentity();
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };

    std::vector<Mesh*> scene;

    struct rRot { float x; float y; float z; }; // Structure to store random rotation parameters
    std::vector<rRot> rotations;

    RandomNumberGenerator& rng = RandomNumberGenerator::getInstance();

    // Create a grid of cubes with random rotations
    for (unsigned int y = 0; y < 6; y++) {
        for (unsigned int x = 0; x < 8; x++) {
            Mesh* m = new Mesh();
            *m = Mesh::makeCube(1.f);
            scene.push_back(m);
            m->world = matrix::makeTranslation(-7.0f + (static_cast<float>(x) * 2.f), 5.0f - (static_cast<float>(y) * 2.f), -8.f);
            rRot r{ rng.getRandomFloat(-.1f, .1f), rng.getRandomFloat(-.1f, .1f), rng.getRandomFloat(-.1f, .1f) };
            rotations.push_back(r);
        }
    }

    // Create a sphere and add it to the scene
    Mesh* sphere = new Mesh();
    *sphere = Mesh::makeSphere(1.0f, 10, 20);
    scene.push_back(sphere);
    float sphereOffset = -6.f;
    float sphereStep = 0.1f;
    sphere->world = matrix::makeTranslation(sphereOffset, 0.f, -6.f);

    auto start = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> end;
    int cycle = 0;

    bool running = true;
    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        // Rotate each cube in the grid
        for (unsigned int i = 0; i < rotations.size(); i++)
            scene[i]->world = scene[i]->world * matrix::makeRotateXYZ(rotations[i].x, rotations[i].y, rotations[i].z);

        // Move the sphere back and forth
        sphereOffset += sphereStep;
        sphere->world = matrix::makeTranslation(sphereOffset, 0.f, -6.f);
        if (sphereOffset > 6.0f || sphereOffset < -6.0f) {
            sphereStep *= -1.f;
            if (++cycle % 2 == 0) {
                end = std::chrono::high_resolution_clock::now();
                std::cout << cycle / 2 << " :" << std::chrono::duration<double, std::milli>(end - start).count() << "ms\n";
                start = std::chrono::high_resolution_clock::now();
            }
        }

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;

        pipeline.run(renderer, scene, camera, L);
        renderer.present();
    }

    for (auto& m : scene)
        delete m;
}

//Scene 3 - wave (cube move up down sin, colour based off height)
void scene3() {
    struct Cube {
        Mesh* mesh;
        float x, z;
        float distance;
    };
    ThreadSys pipeline;
    Renderer renderer;
    // create light source
    Light L{ vec4(0.f, 1.f, 1.f, 0.f), colour(1.0f, 1.0f, 1.0f), colour(0.2f, 0.2f, 0.2f) };


    std::vector<Cube> waveGrid;

    //creating grid (size grid x grid)
    int gridSize = 25;
    //gap size beteween each cube
    float cubeGap = 2.0f;
    //center
    float offset = (gridSize * cubeGap) / 2.0f;

    for (int x = 0; x < gridSize; x++) {
        for (int z = 0; z < gridSize; z++) {
            Cube cube;
            cube.mesh = new Mesh();
            *cube.mesh = Mesh::makeCube(2.0f);

            //calc cubepos
            cube.x = (x * cubeGap) - offset;
            cube.z = (z * cubeGap) - offset;


            //Note to self - this is removing caalls per frame
            cube.distance = sqrt(cube.x * cube.x + cube.z * cube.z);

            waveGrid.push_back(cube);
        }
    }

    auto start = std::chrono::high_resolution_clock::now();
    int cycle = 1;
    float rotSpeed = 0.5f;

    float time = 0.0f;

    bool running = true;

    while (running) {
        renderer.canvas.checkInput();
        renderer.clear();

        if (renderer.canvas.keyPressed(VK_ESCAPE)) break;

        time += 0.016f;

        if ((time * rotSpeed) > (2.0f * M_PI)) {

            auto end = std::chrono::high_resolution_clock::now();
            std::cout << cycle << " :" << std::chrono::duration<double, std::milli>(end - start).count() << "ms" << std::endl;

            //reset
            start = std::chrono::high_resolution_clock::now();
            cycle++;

            time = 0.0f;
        }

        //set back from mid pt
        float radius = 50.0f;
        //camera back pt down (rot cam around wave)
        matrix camera = matrix::makeTranslation(0, -5.0f, -radius) * matrix::makeRotateX(0.5f) * matrix::makeRotateY(time * rotSpeed);

        //prevent write to every part of grid (slightly dif to other scenes)
        std::vector<Mesh*> meshW;
        meshW.reserve(waveGrid.size());


        for (auto& cube : waveGrid) {


            float height = sin(cube.distance - (3.0f * time)) * 2.0f;


            //colour cubes based on height (if higher lighter to sim wave)
            float colourMap = (height + 2.0f) / 4.0f;
            colour c(0.3f * colourMap, 0.6f * colourMap, 1.0f);
            //loop through colour each
            for (auto& v : cube.mesh->vertices) {
                v.rgb = c;
            }

            //apply height translate per frame
            cube.mesh->world = matrix::makeTranslation(cube.x, height, cube.z);
            meshW.push_back(cube.mesh);
        }

        pipeline.run(renderer, meshW, camera, L);
        renderer.present();
    }

    for (auto& cube : waveGrid) {
        delete cube.mesh;
    }
}



// Entry point of the application
// No input variables
int main() {
    scene3();
    //scene2();
    //scene3();
    //sceneTest(); 


    return 0;
}