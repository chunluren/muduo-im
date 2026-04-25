FROM python:3.11-slim
RUN pip install --no-cache-dir Pillow redis
COPY worker.py /app/worker.py
WORKDIR /app
CMD ["python3", "worker.py"]
