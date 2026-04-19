#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstdlib>

static void glfw_error_callback(int error, const char *description)
{
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

int main(int argc, char **argv)
{
    // --- Run tests if requested ---------------------------------------------
    doctest::Context ctx;
    ctx.applyCommandLine(argc, argv);

    // --test   → run tests then exit
    // --no-run → run tests only (doctest flag, suppresses normal program)
    bool run_tests = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string_view(argv[i]) == "--test") {
            run_tests = true;
        }
    }

    if (run_tests) {
        return ctx.run();
    }

    // --- Normal application -------------------------------------------------
    glfwSetErrorCallback(glfw_error_callback);

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        return EXIT_FAILURE;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1024, 768, "Tawny", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        return EXIT_FAILURE;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    int version = gladLoadGL(glfwGetProcAddress);
    if (!version) {
        std::fprintf(stderr, "Failed to load OpenGL\n");
        glfwDestroyWindow(window);
        glfwTerminate();
        return EXIT_FAILURE;
    }

    std::printf("Tawny — OpenGL %d.%d\n",
                GLAD_VERSION_MAJOR(version), GLAD_VERSION_MINOR(version));

    while (!glfwWindowShouldClose(window)) {
        glClearColor(0.0f, 0.0f, 0.2f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// In-source test example
// ---------------------------------------------------------------------------
TEST_CASE("smoke test") {
    CHECK(1 + 1 == 2);
}
