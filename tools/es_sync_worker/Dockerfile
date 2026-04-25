FROM python:3.11-slim
RUN pip install --no-cache-dir pymysql redis
COPY sync.py /app/sync.py
WORKDIR /app
CMD ["python3", "sync.py"]
