# syntax=docker/dockerfile:1.7
# Piper TTS image. The rhasspy project does not publish an HTTP-server
# image, so we bake `piper-tts` into a slim Python base ourselves. This
# avoids the per-boot `pip install` the original draft relied on.

ARG PIPER_VERSION=1.4.2

FROM python:3.12-slim
ARG PIPER_VERSION
RUN pip install --no-cache-dir "piper-tts==${PIPER_VERSION}" flask
EXPOSE 8083
ENTRYPOINT ["python", "-m", "piper.http_server"]
CMD ["--host", "0.0.0.0", "--port", "8083"]
