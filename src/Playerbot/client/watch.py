import time
from watchdog.observers import Observer
from watchdog.events import FileSystemEventHandler
from crud import delete_scripts
from scripts import upload_scripts, get_name_from_path


class Handler(FileSystemEventHandler):
    def __init__(self, config, account_id) -> None:
        super().__init__()
        self.config = config
        self.account_id = account_id
        self.sends = {}

    def report_change(self, file_path, is_delete=False):
        name = get_name_from_path(self.config, file_path)
        pretty_name = f"entrypoint script 'main'" if name == "main" else f"module '{name}'"
        print(f"{'Deployed removal of' if is_delete else 'Deployed'} {pretty_name}")

    def send_change(self, file_path, is_delete=False):
        if not file_path.endswith(".lua"):
            return

        if is_delete:
            delete_scripts(self.config["API_HOST"], [file_path], self.account_id)
            self.report_change(file_path, True)
            return

        if file_path in self.sends:
            if time.time() - self.sends[file_path] < int(self.config["WATCHER_DEPLOY_THROTTLE_MS"]) / 1000:
                return

        upload_scripts(self.config, [file_path], self.account_id, False)
        self.report_change(file_path)
        self.sends[file_path] = time.time()

    def on_created(self, event):
        self.send_change(event.src_path)
        return super().on_created(event)

    def on_modified(self, event):
        self.send_change(event.src_path)
        return super().on_modified(event)

    def on_deleted(self, event):
        self.send_change(event.src_path, True)
        return super().on_deleted(event)

    def on_moved(self, event):
        self.send_change(event.src_path)
        return super().on_moved(event)


def setup_file_watch(config, account_id):
    event_handler = Handler(config, account_id)
    observer = Observer()
    observer.schedule(event_handler, config["SRC_DIR"], recursive=True)
    observer.start()
