// PushMPC installer and updater: JSON, release manifests, the manifest signature.
#include "pm_common.h"

#include <bcrypt.h>
#include <wincrypt.h>
#include <cstring>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace pm {

// ================================================================== JSON
const JValue* JValue::get(const char* key) const {
    if (type != Object) return nullptr;
    for (auto& kv : o) if (kv.first == key) return &kv.second;
    return nullptr;
}
std::string JValue::str(const char* key, const std::string& def) const {
    const JValue* v = get(key);
    if (!v) return def;
    if (v->type == String) return v->s;
    if (v->type == Number) {
        char b[64]; double n = v->n;
        if (n == double((long long)n)) snprintf(b, sizeof(b), "%lld", (long long)n); else snprintf(b, sizeof(b), "%g", n);
        return b;
    }
    if (v->type == Bool) return v->b ? "true" : "false";
    return def;
}
bool JValue::boolean(const char* key, bool def) const {
    const JValue* v = get(key);
    return v && v->type == Bool ? v->b : def;
}

namespace {
struct Parser {
    const std::string& t; size_t i = 0; std::string err;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(i); return false; }
    bool hex4(unsigned& v) {
        if (i + 4 > t.size()) return false;
        v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t[i++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0'); else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10); else return false;
        }
        return true;
    }
    static void utf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += char(cp);
        else if (cp < 0x800) { o += char(0xC0 | (cp >> 6)); o += char(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += char(0xE0 | (cp >> 12)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
        else { o += char(0xF0 | (cp >> 18)); o += char(0x80 | ((cp >> 12) & 63)); o += char(0x80 | ((cp >> 6) & 63)); o += char(0x80 | (cp & 63)); }
    }
    bool string(std::string& out) {
        if (t[i] != '"') return fail("string expected");
        i++;
        while (i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') { out += c; continue; }
            if (i >= t.size()) return fail("bad escape");
            char e = t[i++];
            switch (e) {
            case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
            case 'r': out += '\r'; break; case 't': out += '\t'; break;
            case 'u': {
                unsigned cp; if (!hex4(cp)) return fail("bad \\u");
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    i += 2; unsigned lo; if (!hex4(lo)) return fail("bad surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp); break;
            }
            default: return fail("bad escape");
            }
        }
        if (i >= t.size()) return fail("unterminated string");
        i++;
        return true;
    }
    bool value(JValue& v, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (i >= t.size()) return fail("value expected");
        char c = t[i];
        if (c == '{') {
            v.type = JValue::Object; i++; ws();
            if (i < t.size() && t[i] == '}') { i++; return true; }
            for (;;) {
                ws(); std::string k; if (!string(k)) return false;
                ws(); if (i >= t.size() || t[i] != ':') return fail("':' expected"); i++;
                JValue child; if (!value(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                ws(); if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; return true; }
                return fail("',' or '}' expected");
            }
        }
        if (c == '[') {
            v.type = JValue::Array; i++; ws();
            if (i < t.size() && t[i] == ']') { i++; return true; }
            for (;;) {
                JValue child; if (!value(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                ws(); if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; return true; }
                return fail("',' or ']' expected");
            }
        }
        if (c == '"') { v.type = JValue::String; return string(v.s); }
        if (!t.compare(i, 4, "true")) { v.type = JValue::Bool; v.b = true; i += 4; return true; }
        if (!t.compare(i, 5, "false")) { v.type = JValue::Bool; v.b = false; i += 5; return true; }
        if (!t.compare(i, 4, "null")) { v.type = JValue::Null; i += 4; return true; }
        size_t s = i;
        while (i < t.size() && (strchr("+-.eE", t[i]) || (t[i] >= '0' && t[i] <= '9'))) i++;
        if (s == i) return fail("unexpected character");
        v.type = JValue::Number; v.n = strtod(t.substr(s, i - s).c_str(), nullptr);
        return true;
    }
};
}

bool JsonParse(const std::string& text0, JValue& out, std::string& err) {
    std::string text = text0;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB) text = text.substr(3);
    Parser p(text);
    out = JValue();
    if (!p.value(out, 0)) { err = p.err; return false; }
    p.ws();
    if (p.i != text.size()) { err = "trailing data"; return false; }
    return true;
}
std::string JsonEscape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"': o += "\\\""; break; case '\\': o += "\\\\"; break; case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break; case '\t': o += "\\t"; break;
        default: if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; } else o += c;
        }
    }
    return o;
}

// ================================================================== release.json
const ReleaseComponent* Release::find(const std::string& name, const std::string& vendor) const {
    const ReleaseComponent* any = nullptr;
    for (auto& c : components) {
        if (c.name != name) continue;
        if (vendor.empty() || c.vendor.empty()) { if (!any) any = &c; if (vendor.empty()) return &c; continue; }
        if (_stricmp(c.vendor.c_str(), vendor.c_str()) == 0) return &c;
    }
    return any;   // schema 2: one runtime entry without a vendor
}
bool ParseRelease(const std::string& json, Release& out, std::string& err) {
    JValue root;
    if (!JsonParse(json, root, err)) return false;
    if (root.type != JValue::Object) { err = "not an object"; return false; }
    out = Release();
    out.schema = atoi(root.str("schema", "0").c_str());
    out.release = root.str("release");
    out.tag = root.str("tag");
    out.repository = root.str("repository");
    out.channel = root.str("channel");
    if (out.schema < 2) { err = "unsupported schema " + std::to_string(out.schema); return false; }
    if (out.release.empty()) { err = "no release version"; return false; }
    const JValue* comps = root.get("components");
    if (!comps || comps->type != JValue::Array) { err = "no components"; return false; }
    for (auto& c : comps->a) {
        ReleaseComponent rc;
        rc.name = c.str("name"); rc.version = c.str("version"); rc.url = c.str("url"); rc.sha256 = c.str("sha256");
        rc.vendor = c.str("vendor"); rc.format = c.str("format"); rc.asset = c.str("asset"); rc.subset = c.str("subset");
        rc.repository = c.str("repository"); rc.tag = c.str("tag"); rc.pushfps = c.str("pushfps");
        if (rc.name.empty() || rc.version.empty() || rc.url.empty() || rc.sha256.size() != 64) { err = "incomplete component " + rc.name; return false; }
        out.components.push_back(rc);
    }
    return true;
}

// ================================================================== signature
namespace {
bool B64(const std::string& in, std::vector<unsigned char>& out) {
    std::string s; for (char c : in) if (!isspace((unsigned char)c)) s += c;
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr)) return false;
    out.resize(n);
    return CryptStringToBinaryA(s.c_str(), DWORD(s.size()), CRYPT_STRING_BASE64, out.data(), &n, nullptr, nullptr) != 0;
}
std::string Tag(const std::string& xml, const char* name) {
    std::string open = std::string("<") + name + ">", close = std::string("</") + name + ">";
    size_t a = xml.find(open); if (a == std::string::npos) return {};
    a += open.size();
    size_t b = xml.find(close, a); if (b == std::string::npos) return {};
    return xml.substr(a, b - a);
}
}
bool VerifySignature(const std::string& data, const std::string& sigBase64, const std::string& keyXml, std::wstring& err) {
    std::vector<unsigned char> mod, exp, sig;
    if (!B64(Tag(keyXml, "Modulus"), mod) || !B64(Tag(keyXml, "Exponent"), exp) || mod.empty() || exp.empty()) { err = L"the release key is not an RSAKeyValue"; return false; }
    if (!B64(sigBase64, sig) || sig.empty()) { err = L"the signature is not base64"; return false; }
    // BCRYPT_RSAKEY_BLOB + exponent + modulus, both big-endian (as in the XML)
    std::vector<unsigned char> blob(sizeof(BCRYPT_RSAKEY_BLOB) + exp.size() + mod.size());
    auto* h = reinterpret_cast<BCRYPT_RSAKEY_BLOB*>(blob.data());
    h->Magic = BCRYPT_RSAPUBLIC_MAGIC; h->BitLength = ULONG(mod.size() * 8); h->cbPublicExp = ULONG(exp.size()); h->cbModulus = ULONG(mod.size());
    h->cbPrime1 = h->cbPrime2 = 0;
    memcpy(blob.data() + sizeof(*h), exp.data(), exp.size());
    memcpy(blob.data() + sizeof(*h) + exp.size(), mod.data(), mod.size());
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_RSA_ALGORITHM, nullptr, 0) == 0 &&
        BCryptImportKeyPair(alg, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key, blob.data(), ULONG(blob.size()), 0) == 0) {
        unsigned char digest[32];
        BCRYPT_ALG_HANDLE sha = nullptr;
        if (BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
            BCryptHash(sha, nullptr, 0, (PUCHAR)data.data(), ULONG(data.size()), digest, 32);
            BCryptCloseAlgorithmProvider(sha, 0);
            BCRYPT_PKCS1_PADDING_INFO pad{ BCRYPT_SHA256_ALGORITHM };
            ok = BCryptVerifySignature(key, &pad, digest, 32, sig.data(), ULONG(sig.size()), BCRYPT_PAD_PKCS1) == 0;
            if (!ok) err = L"the signature does not match the release key";
        }
    } else err = L"the release key could not be imported";
    if (key) BCryptDestroyKey(key);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

} // namespace pm
