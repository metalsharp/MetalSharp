const assert = require("node:assert/strict");
const test = require("node:test");
const fastURI = require("fast-uri");

test("rejects hosts with unbalanced authority brackets", () => {
  const uri = "http://user@[@127.0.0.1:8123/admin";

  assert.equal(fastURI.parse(uri).error, "URI host is malformed.");
  assert.equal(fastURI.normalize(uri), uri);
  assert.equal(fastURI.equal(uri, uri), false);
  assert.throws(() => fastURI.resolve("http://example.com/", uri), /URI host is malformed\./);
});
