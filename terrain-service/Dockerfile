FROM python:3.12-slim

WORKDIR /app

# Copy terrain-service code
COPY terrain-service/ .

# Install dependencies including gunicorn
RUN pip install --no-cache-dir -r requirements.txt gunicorn

# Create cache volume mount point
RUN mkdir -p /data/tile-cache

# Expose Flask/gunicorn port
EXPOSE 8000

# Default environment
ENV TERRAIN_CACHE_DIR=/data/tile-cache
ENV TERRAIN_PROVIDER=synthetic

# Run Flask app via gunicorn
CMD ["gunicorn", "--bind", "0.0.0.0:8000", "--workers", "2", "terrain_service.app:create_app()"]

# Volume for persistent tile cache
VOLUME ["/data/tile-cache"]
