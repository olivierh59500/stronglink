#define _GNU_SOURCE
#include <yajl/yajl_tree.h>
#include <limits.h>
#include <sys/mman.h> /* TODO: Portable wrapper */
#include "../deps/sundown/src/markdown.h"
#include "../deps/sundown/html/html.h"
#include "common.h"
#include "async.h"
#include "EarthFS.h"
#include "Template.h"
#include "http/HTTPServer.h"
#include "http/MultipartForm.h"
#include "http/QueryString.h"

#define RESULTS_MAX 50
#define PENDING_MAX 4
#define BUFFER_SIZE (1024 * 8)

typedef struct {
	strarg_t cookie;
	strarg_t content_type;
} BlogHTTPHeaders;
static strarg_t const BlogHTTPFields[] = {
	"cookie",
	"content-type",
};
typedef struct {
	strarg_t content_type;
	strarg_t content_disposition;
} BlogSubmissionHeaders;
static strarg_t const BlogSubmissionFields[] = {
	"content-type",
	"content-disposition",
};

typedef struct Blog* BlogRef;

struct Blog {
	EFSRepoRef repo;

	str_t *dir;
	str_t *staticDir;
	str_t *templateDir;
	str_t *cacheDir;

	TemplateRef header;
	TemplateRef footer;
	TemplateRef entry_start;
	TemplateRef entry_end;
	TemplateRef preview;
	TemplateRef empty;
	TemplateRef compose;

	async_mutex_t *pending_mutex;
	async_cond_t *pending_cond;
	strarg_t pending[PENDING_MAX];
};

// TODO: Real public API.
bool_t URIPath(strarg_t const URI, strarg_t const path, strarg_t *const qs);

static bool_t emptystr(strarg_t const str) {
	return !str || '\0' == str[0];
}
static str_t *BlogCopyPreviewPath(BlogRef const blog, strarg_t const hash) {
	str_t *path;
	if(asprintf(&path, "%s/%.2s/%s", blog->cacheDir, hash, hash) < 0) return NULL;
	return path;
}

struct markdown_state {
	struct html_renderopt opts;
	int (*autolink)(struct buf *ob, const struct buf *link, enum mkd_autolink type, void *opaque);
	int (*link)(struct buf *ob, const struct buf *link, const struct buf *title, const struct buf *content, void *opaque);
};
static int markdown_link(struct buf *ob, const struct buf *link, const struct buf *title, const struct buf *content, void *opaque) {
	struct markdown_state *const state = opaque;
	if(0 != bufprefix(link, "hash://")) {
		return state->link(ob, link, title, content, opaque);
	}
	// TODO: Query string escaping
	struct buf *rel = bufnew(strlen("?q=")+link->size);
	bufputs(rel, "?q=");
	bufput(rel, link->data, link->size);
	int const r = state->link(ob, rel, title, content, opaque);
	bufrelease(rel);

	bufputs(ob, "<sup>[");
	struct buf icon = BUF_STATIC("#");
	struct buf info = BUF_STATIC("Hash address");
	state->link(ob, link, &info, &icon, opaque);
	bufputs(ob, "]</sup>");

	return r;
}
static int markdown_autolink(struct buf *ob, const struct buf *link, enum mkd_autolink type, void *opaque) {
	struct markdown_state *const state = opaque;
	if(MKDA_NORMAL != type) {
		return state->autolink(ob, link, type, opaque);
	}
	str_t *decoded = QSUnescape((strarg_t)link->data, link->size, false);
	struct buf content = BUF_VOLATILE(decoded);
	int const rc = markdown_link(ob, link, NULL, &content, opaque);
	FREE(&decoded);
	return rc;
}

static err_t genMarkdownPreview(BlogRef const blog, EFSSessionRef const session, strarg_t const URI, strarg_t const tmp, EFSFileInfo const *const info) {
	if(
		0 != strcasecmp("text/markdown; charset=utf-8", info->type) &&
		0 != strcasecmp("text/markdown", info->type)
	) return -1; // TODO: Other types, plugins, w/e.

	if(info->size > 1024 * 1024 * 1) return -1;

	async_pool_enter(NULL);
	uv_file file = -1;
	byte_t const *buf = NULL;

	file = async_fs_open(tmp, O_CREAT | O_EXCL | O_RDWR, 0400);
	if(file < 0) goto err;

	// TODO: Portable wrapper
	int fd = open(info->path, O_RDONLY, 0000);
	buf = mmap(NULL, info->size, PROT_READ, MAP_SHARED, fd, 0);
	close(fd); fd = -1;
	if(!buf) goto err;

	struct sd_callbacks callbacks;
	struct markdown_state state;
	sdhtml_renderer(&callbacks, &state.opts, HTML_ESCAPE | HTML_HARD_WRAP);
	state.link = callbacks.link;
	state.autolink = callbacks.autolink;
	callbacks.link = markdown_link;
	callbacks.autolink = markdown_autolink;
	struct sd_markdown *parser = sd_markdown_new(MKDEXT_AUTOLINK | MKDEXT_FENCED_CODE | MKDEXT_NO_INTRA_EMPHASIS | MKDEXT_SUPERSCRIPT, 10, &callbacks, &state);
	struct buf *out = bufnew(1024 * 8);
	sd_markdown_render(out, buf, info->size, parser);
	sd_markdown_free(parser); parser = NULL;

	int written = 0;
	for(;;) {
		uv_buf_t bufs = uv_buf_init((char *)out->data+written, out->size-written);
		int const r = async_fs_write(file, &bufs, 1, 0);
		if(r < 0) goto err;
		written += r;
		if(written >= out->size) break;
	}
	if(async_fs_fdatasync(file) < 0) goto err;

	int const close_err = async_fs_close(file); file = -1;
	if(close_err < 0) goto err;
	munmap((byte_t *)buf, info->size); buf = NULL;
	async_pool_leave(NULL);
	return 0;

err:
	async_fs_unlink(tmp);
	async_fs_close(file); file = -1;
	munmap((byte_t *)buf, info->size); buf = NULL;
	async_pool_leave(NULL);
	return -1;
}
static err_t genPlainTextPreview(BlogRef const blog, EFSSessionRef const session, strarg_t const URI, strarg_t const tmp, EFSFileInfo const *const info) {
	if(
		0 != strcasecmp("text/plain; charset=utf-8", info->type) &&
		0 != strcasecmp("text/plain", info->type)
	) return -1; // TODO: Other types, plugins, w/e.

	// TODO
	return -1;
}


typedef struct {
	BlogRef blog;
	strarg_t fileURI;
} preview_state;
static str_t *preview_metadata(preview_state const *const state, strarg_t const var) {
	strarg_t unsafe = NULL;
	str_t buf[URI_MAX];
	if(0 == strcmp(var, "rawURI")) {
		str_t algo[EFS_ALGO_SIZE]; // EFS_INTERNAL_ALGO
		str_t hash[EFS_HASH_SIZE];
		EFSParseURI(state->fileURI, algo, hash);
		snprintf(buf, sizeof(buf), "/efs/file/%s/%s", algo, hash);
		unsafe = buf;
	}
	if(0 == strcmp(var, "queryURI")) {
		// TODO: Query string escaping
		snprintf(buf, sizeof(buf), "/?q=%s", state->fileURI);
		unsafe = buf;
	}
	if(0 == strcmp(var, "hashURI")) {
		unsafe = state->fileURI;
	}
	if(unsafe) return htmlenc(unsafe);

	EFSConnection const *conn = EFSRepoDBOpen(state->blog->repo);
	int rc;
	MDB_txn *txn = NULL;
	rc = mdb_txn_begin(conn->env, NULL, MDB_RDONLY, &txn);
	assert(MDB_SUCCESS == rc);

	MDB_cursor *metaFiles = NULL;
	rc = mdb_cursor_open(txn, conn->metaFileIDByTargetURI, &metaFiles);
	assert(MDB_SUCCESS == rc);

	MDB_cursor *values = NULL;
	rc = mdb_cursor_open(txn, conn->valueByMetaFileIDField, &values);
	assert(MDB_SUCCESS == rc);

	uint64_t const targetURI_id = db_string_id(txn, conn->schema, state->fileURI);
	uint64_t const field_id = db_string_id(txn, conn->schema, var);

	DB_VAL(targetURI_val, 1);
	db_bind(targetURI_val, targetURI_id);
	MDB_val metaFileID_val[1];
	rc = mdb_cursor_get(metaFiles, targetURI_val, metaFileID_val, MDB_SET);
	assert(MDB_SUCCESS == rc || MDB_NOTFOUND == rc);
	for(; MDB_SUCCESS == rc; rc = mdb_cursor_get(metaFiles, targetURI_val, metaFileID_val, MDB_NEXT_DUP)) {
		uint64_t const metaFileID = db_column(metaFileID_val, 0);
		DB_VAL(metaFileIDField_val, 2);
		db_bind(metaFileIDField_val, metaFileID);
		db_bind(metaFileIDField_val, field_id);

		MDB_val value_val[1];
		rc = mdb_cursor_get(values, metaFileIDField_val, value_val, MDB_SET);
		assert(MDB_SUCCESS == rc || MDB_NOTFOUND == rc);
		for(; MDB_SUCCESS == rc; rc = mdb_cursor_get(values, metaFileIDField_val, value_val, MDB_NEXT_DUP)) {
			strarg_t const value = db_column_text(txn, conn->schema, value_val, 0);
			if(0 == strcmp("", value)) continue;
			unsafe = value;
			break;
		}
		if(unsafe) break;
	}

	mdb_cursor_close(values); values = NULL;
	mdb_cursor_close(metaFiles); metaFiles = NULL;

	if(!unsafe) {
		if(0 == strcmp(var, "thumbnailURI")) unsafe = "/file.png";
		if(0 == strcmp(var, "title")) unsafe = "(no title)";
		if(0 == strcmp(var, "description")) unsafe = "(no description)";
	}
	str_t *result = htmlenc(unsafe);

	mdb_txn_abort(txn); txn = NULL;
	EFSRepoDBClose(state->blog->repo, &conn);
	return result;
}
static void preview_free(preview_state const *const state, strarg_t const var, str_t **const val) {
	FREE(val);
}
static TemplateArgCBs const preview_cbs = {
	.lookup = (str_t *(*)())preview_metadata,
	.free = (void (*)())preview_free,
};

static err_t genGenericPreview(BlogRef const blog, EFSSessionRef const session, strarg_t const URI, strarg_t const tmp, EFSFileInfo const *const info) {
	uv_file file = async_fs_open(tmp, O_CREAT | O_EXCL | O_WRONLY, 0400);
	if(file < 0) return -1;

	preview_state const state = {
		.blog = blog,
		.fileURI = URI,
	};
	err_t const e1 = TemplateWriteFile(blog->preview, &preview_cbs, &state, file);

	err_t const e2 = async_fs_close(file); file = -1;
	if(e1 < 0 || e2 < 0) {
		async_fs_unlink(tmp);
		return -1;
	}
	return 0;
}

static err_t genPreview(BlogRef const blog, EFSSessionRef const session, strarg_t const URI, strarg_t const path) {
	EFSFileInfo info[1];
	if(EFSSessionGetFileInfo(session, URI, info) < 0) return -1;
	str_t *tmp = EFSRepoCopyTempPath(blog->repo);
	if(!tmp) {
		EFSFileInfoCleanup(info);
		return -1;
	}

	bool_t success = false;
	success = success || genMarkdownPreview(blog, session, URI, tmp, info) >= 0;
	success = success || genPlainTextPreview(blog, session, URI, tmp, info) >= 0;
	success = success || genGenericPreview(blog, session, URI, tmp, info) >= 0;
	if(!success) async_fs_mkdirp_dirname(tmp, 0700);
	success = success || genMarkdownPreview(blog, session, URI, tmp, info) >= 0;
	success = success || genPlainTextPreview(blog, session, URI, tmp, info) >= 0;
	success = success || genGenericPreview(blog, session, URI, tmp, info) >= 0;

	EFSFileInfoCleanup(info);
	if(!success) {
		FREE(&tmp);
		return -1;
	}

	success = false;
	success = success || async_fs_link(tmp, path) >= 0;
	if(!success) async_fs_mkdirp_dirname(path, 0700);
	success = success || async_fs_link(tmp, path) >= 0;

	async_fs_unlink(tmp);
	FREE(&tmp);
	return success ? 0 : -1;
}

static bool_t pending(BlogRef const blog, strarg_t const path) {
	for(index_t i = 0; i < PENDING_MAX; ++i) {
		if(!blog->pending[i]) continue;
		if(0 == strcmp(blog->pending[i], path)) return true;
	}
	return false;
}
static bool_t available(BlogRef const blog, index_t *const x) {
	for(index_t i = 0; i < PENDING_MAX; ++i) {
		if(blog->pending[i]) continue;
		*x = i;
		return true;
	}
	return false;
}
static void sendPreview(BlogRef const blog, HTTPMessageRef const msg, EFSSessionRef const session, strarg_t const URI, strarg_t const path) {
	if(!path) return;

	preview_state const state = {
		.blog = blog,
		.fileURI = URI,
	};
	TemplateWriteHTTPChunk(blog->entry_start, &preview_cbs, &state, msg);

	if(HTTPMessageWriteChunkFile(msg, path) >= 0) {
		TemplateWriteHTTPChunk(blog->entry_end, &preview_cbs, &state, msg);
		return;
	}

	// It's okay to accidentally regenerate a preview
	// It's okay to send an error if another thread tried to gen and failed
	// We want to minimize false positives and false negatives
	// In particular, if a million connections request a new file at once, we want to avoid starting gen for each connection before any of them have finished
	// Capping the total number of concurrent gens to PENDING_MAX is not a bad side effect
	index_t x = 0;
	async_mutex_lock(blog->pending_mutex);
	for(;; async_cond_wait(blog->pending_cond, blog->pending_mutex)) {
		if(pending(blog, path)) { x = PENDING_MAX; continue; }
		if(x >= PENDING_MAX) break;
		if(available(blog, &x)) break;
	}
	async_mutex_unlock(blog->pending_mutex);

	if(x < PENDING_MAX) {
		(void)genPreview(blog, session, URI, path);

		async_mutex_lock(blog->pending_mutex);
		blog->pending[x] = NULL;
		async_cond_broadcast(blog->pending_cond);
		async_mutex_unlock(blog->pending_mutex);
	}

	if(HTTPMessageWriteChunkFile(msg, path) < 0) {
		TemplateWriteHTTPChunk(blog->empty, &preview_cbs, &state, msg);
	}

	TemplateWriteHTTPChunk(blog->entry_end, &preview_cbs, &state, msg);
}

typedef struct {
	str_t *query;
	str_t *file; // It'd be nice if we didn't need a separate parameter for this.
} BlogQueryValues;
static strarg_t const BlogQueryFields[] = {
	"q",
	"f",
};
static bool_t getResultsPage(BlogRef const blog, HTTPMessageRef const msg, HTTPMethod const method, strarg_t const URI) {
	if(HTTP_GET != method) return false;
	strarg_t qs = NULL;
	if(!URIPath(URI, "/", &qs)) return false;

	BlogHTTPHeaders const *const headers = HTTPMessageGetHeaders(msg, BlogHTTPFields, numberof(BlogHTTPFields));
	EFSSessionRef session = EFSRepoCreateSession(blog->repo, headers->cookie);
	if(!session) {
		HTTPMessageSendStatus(msg, 403);
		return true;
	}

	BlogQueryValues *params = QSValuesCopy(qs, BlogQueryFields, numberof(BlogQueryFields));
	EFSFilterRef filter = EFSUserFilterParse(params->query);
	if(!filter) filter = EFSFilterCreate(EFSAllFilterType);
	QSValuesFree((QSValues *)&params, numberof(BlogQueryFields));

	EFSFilterPrint(filter, 0); // DEBUG

	str_t **URIs = EFSSessionCopyFilteredURIs(session, filter, RESULTS_MAX);
	if(!URIs) {
		HTTPMessageSendStatus(msg, 500);
		EFSFilterFree(&filter);
		EFSSessionFree(&session);
		return true;
	}

	HTTPMessageWriteResponse(msg, 200, "OK");
	HTTPMessageWriteHeader(msg, "Content-Type", "text/html; charset=utf-8");
	HTTPMessageWriteHeader(msg, "Transfer-Encoding", "chunked");
	HTTPMessageBeginBody(msg);

	str_t q[200];
	size_t qlen = EFSFilterToUserFilterString(filter, q, sizeof(q), 0);
	str_t *q_HTMLSafe = htmlenc(q);
	TemplateStaticArg const args[] = {
		{"q", q_HTMLSafe},
		{NULL, NULL},
	};

	TemplateWriteHTTPChunk(blog->header, &TemplateStaticCBs, args, msg);

	// TODO: This is pretty broken. We probably need a whole separate mode.
	EFSFilterRef const coreFilter = EFSFilterUnwrap(filter);
	if(coreFilter && EFSMetadataFilterType == EFSFilterGetType(coreFilter)) {
		strarg_t const URI = EFSFilterGetStringArg(coreFilter, 1);
		EFSFileInfo info[1];
		if(EFSSessionGetFileInfo(session, URI, info) >= 0) {
			str_t *canonical = EFSFormatURI(EFS_INTERNAL_ALGO, info->hash);
			str_t *previewPath = BlogCopyPreviewPath(blog, info->hash);
			sendPreview(blog, msg, session, canonical, previewPath);
			FREE(&previewPath);
			FREE(&canonical);
			EFSFileInfoCleanup(info);
			// TODO: Remember this file and make sure it doesn't show up as a duplicate below.
		}
	}

	for(index_t i = 0; URIs[i]; ++i) {
		str_t algo[EFS_ALGO_SIZE]; // EFS_INTERNAL_ALGO
		str_t hash[EFS_HASH_SIZE];
		EFSParseURI(URIs[i], algo, hash);
		str_t *previewPath = BlogCopyPreviewPath(blog, hash);
		sendPreview(blog, msg, session, URIs[i], previewPath);
		FREE(&previewPath);
	}

	TemplateWriteHTTPChunk(blog->footer, &TemplateStaticCBs, args, msg);
	FREE(&q_HTMLSafe);

	HTTPMessageWriteChunkv(msg, NULL, 0);
	HTTPMessageEnd(msg);

	for(index_t i = 0; URIs[i]; ++i) FREE(&URIs[i]);
	FREE(&URIs);
	EFSFilterFree(&filter);
	EFSSessionFree(&session);
	return true;
}
static bool_t getCompose(BlogRef const blog, HTTPMessageRef const msg, HTTPMethod const method, strarg_t const URI) {
	if(HTTP_GET != method) return false;
	if(!URIPath(URI, "/compose", NULL)) return false;

	BlogHTTPHeaders const *const headers = HTTPMessageGetHeaders(msg, BlogHTTPFields, numberof(BlogHTTPFields));
	EFSSessionRef session = EFSRepoCreateSession(blog->repo, headers->cookie);
	if(!session) {
		HTTPMessageSendStatus(msg, 403);
		return true;
	}

	HTTPMessageWriteResponse(msg, 200, "OK");
	HTTPMessageWriteHeader(msg, "Content-Type", "text/html; charset=utf-8");
	HTTPMessageWriteHeader(msg, "Transfer-Encoding", "chunked");
	HTTPMessageBeginBody(msg);
	TemplateWriteHTTPChunk(blog->compose, NULL, NULL, msg);
	HTTPMessageWriteChunkv(msg, NULL, 0);
	HTTPMessageEnd(msg);

	EFSSessionFree(&session);
	return true;
}
static bool_t postSubmission(BlogRef const blog, HTTPMessageRef const msg, HTTPMethod const method, strarg_t const URI) {
	if(HTTP_POST != method) return false;
	if(!URIPath(URI, "/submission", NULL)) return false;

	BlogHTTPHeaders const *const headers = HTTPMessageGetHeaders(msg, BlogHTTPFields, numberof(BlogHTTPFields));
	EFSSessionRef session = EFSRepoCreateSession(blog->repo, headers->cookie);
	if(!session) {
		HTTPMessageSendStatus(msg, 403);
		return true;
	}

	// TODO: CSRF token
	MultipartFormRef form = MultipartFormCreate(msg, headers->content_type, BlogSubmissionFields, numberof(BlogSubmissionFields));
	FormPartRef const part = MultipartFormGetPart(form);
	BlogSubmissionHeaders const *const fheaders = FormPartGetHeaders(part);

	strarg_t type;
	if(0 == strcmp("form-data; name=\"markdown\"", fheaders->content_disposition)) {
		type = "text/markdown; charset=utf-8";
	} else {
		type = fheaders->content_type;
	}

	strarg_t title = NULL; // TODO: Get file name from form part.

	EFSSubmissionRef sub = NULL;
	EFSSubmissionRef meta = NULL;
	if(EFSSubmissionCreatePair(session, type, (ssize_t (*)())FormPartGetBuffer, part, title, &sub, &meta) < 0) {
		HTTPMessageSendStatus(msg, 500);
		MultipartFormFree(&form);
		EFSSessionFree(&session);
		return true;
	}

	EFSSubmissionRef subs[2] = { sub, meta };
	err_t err = EFSSubmissionBatchStore(subs, numberof(subs));

	EFSSubmissionFree(&sub);
	EFSSubmissionFree(&meta);
	MultipartFormFree(&form);
	EFSSessionFree(&session);

	if(err >= 0) {
		HTTPMessageWriteResponse(msg, 303, "See Other");
		HTTPMessageWriteHeader(msg, "Location", "/");
		HTTPMessageWriteContentLength(msg, 0);
		HTTPMessageBeginBody(msg);
		HTTPMessageEnd(msg);
	} else {
		HTTPMessageSendStatus(msg, 500);
	}

	return true;
}


BlogRef BlogCreate(EFSRepoRef const repo) {
	assertf(repo, "Blog requires valid repo");

	BlogRef const blog = calloc(1, sizeof(struct Blog));
	if(!blog) return NULL;
	blog->repo = repo;

	// TODO: Check for failures

	asprintf(&blog->dir, "%s/blog", EFSRepoGetDir(repo));
	asprintf(&blog->staticDir, "%s/static", blog->dir);
	asprintf(&blog->templateDir, "%s/template", blog->dir);
	asprintf(&blog->cacheDir, "%s/blog", EFSRepoGetCacheDir(repo));

	str_t *path = malloc(PATH_MAX);
	snprintf(path, PATH_MAX, "%s/header.html", blog->templateDir);
	blog->header = TemplateCreateFromPath(path);
	snprintf(path, PATH_MAX, "%s/footer.html", blog->templateDir);
	blog->footer = TemplateCreateFromPath(path);
	snprintf(path, PATH_MAX, "%s/entry-start.html", blog->templateDir);
	blog->entry_start = TemplateCreateFromPath(path);
	snprintf(path, PATH_MAX, "%s/entry-end.html", blog->templateDir);
	blog->entry_end = TemplateCreateFromPath(path);
	snprintf(path, PATH_MAX, "%s/preview.html", blog->templateDir);
	blog->preview = TemplateCreateFromPath(path);
	snprintf(path, PATH_MAX, "%s/empty.html", blog->templateDir);
	blog->empty = TemplateCreateFromPath(path);
	snprintf(path, PATH_MAX, "%s/compose.html", blog->templateDir);
	blog->compose = TemplateCreateFromPath(path);
	FREE(&path);

	blog->pending_mutex = async_mutex_create();
	blog->pending_cond = async_cond_create();

	return blog;
}
void BlogFree(BlogRef *const blogptr) {
	BlogRef blog = *blogptr;
	if(!blog) return;

	FREE(&blog->dir);
	FREE(&blog->staticDir);
	FREE(&blog->templateDir);
	FREE(&blog->cacheDir);

	TemplateFree(&blog->header);
	TemplateFree(&blog->footer);
	TemplateFree(&blog->entry_start);
	TemplateFree(&blog->entry_end);
	TemplateFree(&blog->preview);
	TemplateFree(&blog->empty);
	TemplateFree(&blog->compose);

	async_mutex_free(blog->pending_mutex); blog->pending_mutex = NULL;
	async_cond_free(blog->pending_cond); blog->pending_cond = NULL;

	FREE(blogptr); blog = NULL;
}
bool_t BlogDispatch(BlogRef const blog, HTTPMessageRef const msg, HTTPMethod const method, strarg_t const URI) {
	if(getResultsPage(blog, msg, method, URI)) return true;
	if(getCompose(blog, msg, method, URI)) return true;
	if(postSubmission(blog, msg, method, URI)) return true;

	if(HTTP_GET != method && HTTP_HEAD != method) return false;

	// TODO: Ignore query parameters, check for `..` (security critical).
	str_t *path;
	int const plen = asprintf(&path, "%s%s", blog->staticDir, URI);
	if(plen < 0) {
		HTTPMessageSendStatus(msg, 500);
		return true;
	}

	HTTPMessageSendFile(msg, path, NULL, -1); // TODO: Determine file type.
	FREE(&path);
	return true;
}

