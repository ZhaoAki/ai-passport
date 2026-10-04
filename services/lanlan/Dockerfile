# Cyber Lanlan service image.
#
# The service uses the Python standard library only, so there is no pip install
# step and no lock file to ship. The database lives on a mounted volume.
#
# Build from the repository root:
#   docker build -f services/lanlan/Dockerfile -t lanlan-service .
FROM python:3.12-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONPATH=/srv/lanlan/services \
    LANLAN_HOST=0.0.0.0 \
    LANLAN_PORT=8787 \
    LANLAN_DB=/data/lanlan.sqlite3 \
    LANLAN_ENV=production \
    LANLAN_SECURE_COOKIES=1 \
    LANLAN_WEB_DIR=/srv/lanlan/web/lanlan

WORKDIR /srv/lanlan

# Only the service package and the static page are needed at runtime.
COPY services/lanlan /srv/lanlan/services/lanlan
COPY web/lanlan /srv/lanlan/web/lanlan

RUN useradd --system --create-home --uid 10001 lanlan \
    && mkdir -p /data \
    && chown -R lanlan:lanlan /srv/lanlan /data \
    && rm -rf /srv/lanlan/services/lanlan/tests /srv/lanlan/services/lanlan/__pycache__

USER lanlan

VOLUME ["/data"]
EXPOSE 8787

# The health check requests a static file, so it never touches family data.
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
    CMD ["python3", "-c", "import urllib.request,sys; sys.exit(0 if urllib.request.urlopen('http://127.0.0.1:8787/index.html', timeout=4).status == 200 else 1)"]

ENTRYPOINT ["python3", "-m", "lanlan"]
CMD ["serve"]
