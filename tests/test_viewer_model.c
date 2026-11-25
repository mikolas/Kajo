#include "../src/viewer/viewer_model.h"
#include "../src/ide/ide_markdown.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

static void
test_classify_path_and_mime(void)
{
    g_assert_cmpint(viewer_model_classify_path("/docs/spec.PDF", NULL), ==, VIEWER_KIND_PDF);
    g_assert_cmpint(viewer_model_classify_path("/docs/ROADMAP.md", NULL), ==, VIEWER_KIND_MARKDOWN);
    g_assert_cmpint(viewer_model_classify_path("/docs/guide.markdown#sec1", NULL), ==, VIEWER_KIND_MARKDOWN);
    g_assert_cmpint(viewer_model_classify_path("/pics/banner.svg", NULL), ==, VIEWER_KIND_SVG);
    g_assert_cmpint(viewer_model_classify_path("/pics/icon.SVGZ", NULL), ==, VIEWER_KIND_SVG);
    g_assert_cmpint(viewer_model_classify_path("/pics/photo.avif", NULL), ==, VIEWER_KIND_IMAGE);
    g_assert_cmpint(viewer_model_classify_path("/pics/shot.jxl", NULL), ==, VIEWER_KIND_IMAGE);
    g_assert_cmpint(viewer_model_classify_path("/pics/camera.HEIC", NULL), ==, VIEWER_KIND_IMAGE);

    /* MIME fallback for extensionless files */
    g_assert_cmpint(viewer_model_classify_path("/tmp/blob", "application/pdf"), ==, VIEWER_KIND_PDF);
    g_assert_cmpint(viewer_model_classify_path("/tmp/blob", "text/markdown"), ==, VIEWER_KIND_MARKDOWN);
    g_assert_cmpint(viewer_model_classify_path("/tmp/blob", "image/svg+xml"), ==, VIEWER_KIND_SVG);
    g_assert_cmpint(viewer_model_classify_path("/tmp/blob", "image/webp"), ==, VIEWER_KIND_IMAGE);
    g_assert_cmpint(viewer_model_classify_path("/tmp/blob", "application/octet-stream"), ==, VIEWER_KIND_NONE);

    g_assert_true(viewer_model_is_image_kind(VIEWER_KIND_IMAGE));
    g_assert_true(viewer_model_is_image_kind(VIEWER_KIND_SVG));
    g_assert_false(viewer_model_is_image_kind(VIEWER_KIND_PDF));
}

static void
test_sibling_image_scan_natural_sort(void)
{
    g_autofree char *tmp_dir = g_dir_make_tmp("kajo-viewer-sib-XXXXXX", NULL);
    g_assert_nonnull(tmp_dir);

    const char *names[] = {
        "img10.png", "img2.png", "notes.md", "banner.svg", "img1.jpg", "spec.pdf"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char *p = g_build_filename(tmp_dir, names[i], NULL);
        g_assert_true(g_file_set_contents(p, "x", 1, NULL));
    }

    g_autofree char *cur = g_build_filename(tmp_dir, "img2.png", NULL);
    int cur_idx = -1;
    GPtrArray *sibs = viewer_model_scan_sibling_images(cur, &cur_idx);
    g_assert_nonnull(sibs);
    g_assert_cmpuint(sibs->len, ==, 4);

    /* banner.svg < img1.jpg < img2.png < img10.png */
    g_assert_true(g_str_has_suffix((const char *)g_ptr_array_index(sibs, 0), "banner.svg"));
    g_assert_true(g_str_has_suffix((const char *)g_ptr_array_index(sibs, 1), "img1.jpg"));
    g_assert_true(g_str_has_suffix((const char *)g_ptr_array_index(sibs, 2), "img2.png"));
    g_assert_true(g_str_has_suffix((const char *)g_ptr_array_index(sibs, 3), "img10.png"));
    g_assert_cmpint(cur_idx, ==, 2);

    g_ptr_array_free(sibs, TRUE);

    for (size_t i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autofree char *p = g_build_filename(tmp_dir, names[i], NULL);
        g_unlink(p);
    }
    g_rmdir(tmp_dir);
}

static void
test_navigation_history(void)
{
    ViewerHistory *h = viewer_history_new();
    g_assert_false(viewer_history_can_back(h));
    g_assert_false(viewer_history_can_forward(h));

    viewer_history_push(h, "/a.md", 0, 0.0);
    viewer_history_push(h, "/b.pdf", 3, 0.25);
    viewer_history_push(h, "/c.png", 0, 0.0);

    g_assert_cmpstr(viewer_history_current(h)->path, ==, "/c.png");
    g_assert_true(viewer_history_can_back(h));
    g_assert_false(viewer_history_can_forward(h));

    const ViewerHistoryEntry *e = viewer_history_back(h);
    g_assert_nonnull(e);
    g_assert_cmpstr(e->path, ==, "/b.pdf");
    g_assert_cmpint(e->page_index, ==, 3);
    g_assert_true(viewer_history_can_forward(h));

    /* Branch navigation truncates forward stack (/c.png) */
    viewer_history_push(h, "/d.md", 0, 0.5);
    g_assert_false(viewer_history_can_forward(h));
    g_assert_cmpstr(viewer_history_current(h)->path, ==, "/d.md");

    e = viewer_history_back(h);
    g_assert_cmpstr(e->path, ==, "/b.pdf");
    e = viewer_history_forward(h);
    g_assert_cmpstr(e->path, ==, "/d.md");

    viewer_history_free(h);
}

static void
test_zoom_and_fit_math(void)
{
    double z = viewer_model_step_zoom(1.0, 1, 0.1, 16.0);
    g_assert_cmpfloat(fabs(z - 1.15), <, 0.001);
    z = viewer_model_step_zoom(1.0, -1, 0.1, 16.0);
    g_assert_cmpfloat(fabs(z - 0.85), <, 0.001);
    g_assert_cmpfloat(fabs(viewer_model_step_zoom(16.0, 1, 0.1, 16.0) - 16.0), <, 0.001);

    g_assert_cmpfloat(fabs(viewer_model_compute_fit_scale(1000, 500, 500, 500, VIEWER_FIT_WIDTH) - 0.5), <, 0.001);
    g_assert_cmpfloat(fabs(viewer_model_compute_fit_scale(1000, 2000, 500, 500, VIEWER_FIT_CONTAIN) - 0.25), <, 0.001);
    g_assert_cmpfloat(fabs(viewer_model_compute_fit_scale(1000, 2000, 500, 500, VIEWER_FIT_FILL) - 0.5), <, 0.001);

    char b[32];
    viewer_model_format_bytes(512, b, sizeof(b));
    g_assert_cmpstr(b, ==, "512 B");
    viewer_model_format_bytes(2048, b, sizeof(b));
    g_assert_cmpstr(b, ==, "2.0 KB");
    viewer_model_format_bytes(3u * 1024u * 1024u, b, sizeof(b));
    g_assert_cmpstr(b, ==, "3.0 MB");
}

static void
test_markdown_heading_outline_skips_fences(void)
{
    const char *md =
        "# Title One\n"
        "Some text.\n"
        "```bash\n"
        "# Not a heading inside fence\n"
        "## Also inside fence\n"
        "```\n"
        "## Section Two ##\n"
        "### Subsection Three\n";

    GArray *headings = ide_markdown_extract_headings(md);
    g_assert_nonnull(headings);
    g_assert_cmpuint(headings->len, ==, 3);

    IdeMarkdownHeading *h0 = &g_array_index(headings, IdeMarkdownHeading, 0);
    IdeMarkdownHeading *h1 = &g_array_index(headings, IdeMarkdownHeading, 1);
    IdeMarkdownHeading *h2 = &g_array_index(headings, IdeMarkdownHeading, 2);

    g_assert_cmpuint(h0->level, ==, 1);
    g_assert_cmpuint(h0->line_1based, ==, 1);
    g_assert_cmpstr(h0->title, ==, "Title One");

    g_assert_cmpuint(h1->level, ==, 2);
    g_assert_cmpuint(h1->line_1based, ==, 7);
    g_assert_cmpstr(h1->title, ==, "Section Two");

    g_assert_cmpuint(h2->level, ==, 3);
    g_assert_cmpuint(h2->line_1based, ==, 8);
    g_assert_cmpstr(h2->title, ==, "Subsection Three");

    g_array_unref(headings);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/viewer-model/classify-path-and-mime", test_classify_path_and_mime);
    g_test_add_func("/viewer-model/sibling-image-natural-sort", test_sibling_image_scan_natural_sort);
    g_test_add_func("/viewer-model/navigation-history", test_navigation_history);
    g_test_add_func("/viewer-model/zoom-and-fit-math", test_zoom_and_fit_math);
    g_test_add_func("/viewer-model/markdown-heading-outline", test_markdown_heading_outline_skips_fences);

    return g_test_run();
}