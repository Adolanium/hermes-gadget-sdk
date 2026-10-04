// Older Hermes plugins link to the site root. Keep the device address in the fragment.
if (new URLSearchParams(location.hash.slice(1)).has("server")) {
  location.replace(`installer.html${location.hash}`);
}
