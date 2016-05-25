-- This Source Code Form is subject to the terms of the Mozilla Public
-- License, v. 2.0. If a copy of the MPL was not distributed with this
-- file, You can obtain one at http://mozilla.org/MPL/2.0/.

require "string"
require "cjson"
require "os"

local openssl = require "openssl"
local name = openssl.x509.name
local asn1 = openssl.asn1

local certPrefix = "-----BEGIN CERTIFICATE-----\n"
local certSuffix = "-----END CERTIFICATE-----\n"

local msg = {
  Type = "tls_report",
  Fields = {}
}

-- create PEM data from base64 encoded DER
function make_pem(data)
  local pem = certPrefix
  local offset = 1
  while offset <= data:len() do
    local stop = offset + 63
    if stop > data:len() then
      stop = data:len()
    end
    pem = pem .. data:sub(offset, stop) .. "\n"
    offset = stop + 1
  end
  return pem .. certSuffix
end

-- read and parse a certificate
function read_cert(data)
  local pem = make_pem(data)
  return pcall(openssl.x509.read, pem)
end

function parse_cert(cert)
  return pcall(cert.parse, cert)
end

function process_message()
    local payload = read_message("Fields[submission]")
    local ok, report = pcall(cjson.decode, payload)
    if not ok then return -1, report end

    -- copy over the expected fields
    local expected = {
      "hostname",
      "port",
      "timestamp",
      "errorCode",
      "failedCertChain",
      "userAgent",
      "version",
      "build",
      "product",
      "channel"
    }

    for i, fieldname in ipairs(expected) do
      local field = report[fieldname]
      -- ensure the field is not empty (and does not contain an empty table)
      if nil ~= field then
        if not ("table" == type(field) and next(field) == nil) then
          msg.Fields[fieldname] = field
        end
      end
    end

    -- calculate the clock skew - in seconds, since os.time() returns those
    local reportTime = report["timestamp"]
    if nil ~= reportTime then
      local time = os.time()
      -- skew will be positive if the remote timestamp is in the future
      local skew = reportTime - time

      msg.Fields["skew"] = skew
    end

    -- extract the rootmost and end entity certificates
    local failedCertChain = report["failedCertChain"]
    local ee = nil
    local rootMost = nil
    if nil ~= failedCertChain and nil ~= next(failedCertChain) then
      for i, cert in ipairs(failedCertChain) do
        if nil == ee then
          ee = cert
        end
        rootMost = cert
      end
    end

    -- get the issuer name from the root-most certificate
    if nil ~= rootMost then
      local parsed = nil
      local ok, cert = read_cert(rootMost);
      if ok and nil ~= cert then
        ok, parsed = parse_cert(cert)
      end
      if ok and nil ~= parsed then
        local issuer = parsed["issuer"]
        if nil ~= issuer then
          msg.Fields["rootIssuer"] = issuer:get_text("CN")
        end
      end
    end

    -- determine if the end entity subject or SAN matches the hostname
    local hostname = report["hostname"]
    if nil ~= ee and nil ~= hostname then
      local ok, cert = read_cert(ee);
      if ok and nil ~= cert then
        local ok, matches = pcall(cert.check_host, cert, hostname)
        if ok then
          msg.Fields["hostnameMatch"] = matches
        end
      end
    end

    inject_message(msg)
    return 0
end
